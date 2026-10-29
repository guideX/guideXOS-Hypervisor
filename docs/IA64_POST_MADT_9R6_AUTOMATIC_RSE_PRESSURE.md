# IA64-POST-MADT-9R6 — Automatic RSE Spill/Fill Under Register Pressure

## Outcome

**Outcome A — automatic spill/fill verified for the exercised enforced-lazy paths.** The production decoder now spills the oldest dirty stacked registers when `alloc` would exceed the fixed 96-entry physical ring, and `br.ret` reloads displaced caller values from guest backing memory. Dedicated regressions cover capacity boundaries, repeated nested pressure, RNAT/NaT collection boundaries, memory provenance, partial transfer faults, and a pressure-driven guest A/B switch.

The qualification is bounded. Hardware interruption delivery and restart after an RSE transfer fault are not implemented; the fault behavior and its tear points are documented below. Non-enforced-lazy RSC modes are not qualified by these tests.

The phase gate remains `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`. `.phase` was not advanced.

## Repository preflight and closeout

| Item | Starting state | Closeout |
|---|---|---|
| Repository | `D:\dev\guideXOS_Hypervisor_IA64` | Same |
| Branch | `IA64` | `IA64` |
| HEAD | `ff8df286ea8821c3714cde396b6b6c4697ff75f5` — `Record IA-64 R5 closeout commit` | Implementation and documentation commits are local; the documentation commit SHA is reported in the task response |
| Phase | Phase 8 → 9 | Unchanged |
| Upstream | `origin/IA64`, 2 ahead / 0 behind | No push or remote configuration change |
| Untracked build trees | `build-7r1/`, `build-9r2/`, `build-9r3/`, `build-9r4/`, `build-9r5/` | Preserved |

The required starting revision and phase gate matched. No branch switch, reset, rebase, merge, cherry-pick, clean, stash, or destructive artifact operation was performed. No new serialized RSE state was added, and no checkpoint format version changed.

## Physical-register model and why the 192-register probe pressures it

The emulator configures 128 logical general registers and 32 static registers. The physical stacked-register ring is therefore exactly `NUM_GENERAL_REGISTERS - NUM_STATIC_GR = 96` entries. This is a fixed compile-time capacity, not a runtime option. It is a supported IA-64 capacity: Intel requires at least 96 physical stacked registers and an implementation capacity that is an even multiple of 16.

`GR32..GR127` map into the ring modulo 96. `BOF` supplies the ring base; `CFM.SOR` and `CFM.RRB.GR` rotate the rotating prefix. Architecturally, the current frame occupies the ring window beginning at `BOF` for `CFM.SOF` entries; it is a frame boundary, not a fifth mutually exclusive register class. The emulator's per-entry bookkeeping labels are `CURRENT`, `DIRTY`, `CLEAN`, or `INVALID`. `DIRTY` entries need backing-store writes, `CLEAN` entries have a recoverable backing copy, and `INVALID` entries no longer represent guest state. Because `SetGR` marks a written stacked register `DIRTY`, an active frame can be identified by BOF/SOF while some of its entries carry the DIRTY bookkeeping tag.

`BSP` is the logical end of the register-stack state and `BSPSTORE` is the next backing-store transfer position. Internal `storeReg` and `loadReg` indices track ring transfer positions; `bspLoad` records the guest-memory load frontier used by `loadrs`. Each 512-byte RNAT collection covers 63 register words; the 64th word is its collection slot and is skipped by register-pointer advancement. `AR.RNAT` represents the live collection for its region, with bit 63 reserved.

The old Phase 9R5 probe used a 64-register root and two nested 64-register frames (`alloc`, `br.call`, and `br.ret`), retaining 192 logical frame positions in a 96-entry ring. The sequence genuinely pressures the selected implementation: the root call makes 64 caller registers dirty, so the first nested `alloc 64` requires 64 dirty caller entries plus 64 new frame entries, or 128 positions. At least 32 oldest dirty entries must be written before that allocation can proceed. The next nested 64-register allocation requires another spill wave. An enforced-lazy implementation does not need optional background transfers, but these are capacity-mandated stores.

## Reproduced pre-repair failure

The unchanged R5 executable completed its bounded guest probe in 150 instructions, but reported `BSP=BSPSTORE=0xa000`, `maxBspstore=0xa000`, `spillObserved=no`, and `fillRestored=no`. The sentinel backing-store region was unchanged and the two checked caller values in GR32 and GR64 were incorrect after return. The fixture began with `CFM=0x2040`, `PFS=0`, `BOF=0`, `RSC=0`, `BSP=BSPSTORE=0xa000`, and 64 distinguishable root values (`0xc000000000000000 + register offset`); its backing words were initialized to `0xabababababababab` and `AR.RNAT` to zero. `SetGR` initially labels those 64 physical entries `DIRTY`; the remaining 32 ring entries start `INVALID`.

The first incorrect architectural transition was the first nested `alloc r40=ar.pfs,64,64,0`: after the caller's `br.call`, it needed 32 backing-store writes to preserve the oldest 32 caller registers, but the old implementation changed CFM without spilling. The pointer-defined dirty interval at setup was empty because BSP equaled BSPSTORE, even though `SetGR` had marked 64 physical entries `DIRTY`; after the guest call, BSP advanced over the 64 caller registers. The old executable did not emit a partition-count trace at each instruction, so no post-transition dirty/clean/invalid counts or exact observed bad GR hex values are claimed here. The old probe's failure and its first required capacity transition are reproducible from its unchanged test source and the pre-repair run.

## Implementation

In `src/decoder/decoder.cpp`, `alloc` counts the pointer-defined dirty range from BSPSTORE to BSP and computes the minimum spill needed for `dirtyCount + newSOF <= 96`. It spills only that prefix through the existing IA-64 data translation and guest-memory path, using `storeReg` to map the oldest eligible physical entry. Every successful register write commits its value, its NaT bit into the RNAT accumulator, BSPSTORE, storeReg, and the entry's `CLEAN` status. RNAT collection words are written at slot 63, with bit 63 cleared, and skipped as registers. New frame extension entries are cleared before being made current, preventing values from a prior ring occupant from leaking into the frame. The frame transition follows the required spills.

The existing return-fill path now validates both PFS.SOF and PFS.SOL, reads displaced caller words from guest backing memory, selects the live `AR.RNAT` collection for its region and reads written RNAT collections for other regions, restores NaT bits and physical ring placement, and updates BSPSTORE, load pointer, and ring indices. The return path invalidates callee-only physical entries after preserving slots belonging to the returned-to frame, including slots overlapping after ring wrap. It does not use host call-frame snapshots.

No ring expansion, per-guest shortcut, or additional CPUState field was introduced. The implementation uses the existing 96-entry representation and existing checkpoint serialization layout.

## Regression coverage

`tests/test_ia64_rse_automatic_pressure.cpp` uses the production VM, production IA-64 plugin/decoder, guest-encoded instructions, and one persistent CPUState. It covers:

- No-pressure 32+32 and exact-capacity 32+64 allocations: no eager spill.
- First overflow 33+64: exactly one register stored, followed by a watched guest-memory read on return.
- Nested 64-register frames with repeated unwind/reallocation: observed BSPSTORE frontiers `0xa100`, `0xa308`, and `0xa510`, repeated twice. This accounts for 160 transferred registers and two RNAT collection words.
- Root values and NaTs at offsets 3, 62, and 63; RNAT bits before and after collection boundaries; guest memory contains the spilled values; read watches prove fill provenance; sentinel checks ensure no writes outside the expected transfer range.
- A failed `alloc` spill at the end of RAM: the final valid register and its RNAT collection commit, the next register write faults, BSPSTORE/storeReg remain at the first failed transfer, and CFM/PFS/BSP do not advance.
- A failed `br.ret` fill: a valid caller register and its RNAT-derived NaT load before the next out-of-range read. CFM/PFS/BSP/BSPSTORE and the return target do not commit.

`tests/test_ia64_single_cpu_context_switch.cpp` keeps the existing A → B → A → B → A regression strict: it fails if the pressure probe does not spill, return, or restore its distinguished values. Its synthetic nested routine now writes the `alloc` destination to r37; the prior r38 destination was outside the SOF=6 frame, so the guest fixture itself was not legal. No original A/B assertions were removed. The added bounded integration scenario creates pressure in A, suspends A through the guest switch procedure (`flushrs`, RSC/BSPSTORE updates, guest records and `br.ret`), executes B from its own backing region, switches back to A, observes a read from A's backing region, and unwinds A's chain. Root register values, NaT bits, distinct backing-store contents, and the original A BSP boundary are checked.

The explicit transfer coverage remains separate: `test_new_instructions` exercises `flushrs`/`loadrs`, including RNAT boundary handling, NaT restoration, and load faults; the existing one-CPU guest switch executes `flushrs` and preserves the Phase 9R5 live-RNAT return-fill case.

## Fault and restart limits

The exercised memory-fault paths stop the current VM instruction and put the VM into its error state through the existing exception handler. They do not deliver an IA-64 RSE interruption through `ISR/IFA/ITIR`, select an interruption vector, or restart the faulting `alloc`/`br.ret` instruction. The tests verify the emulator's current tear points rather than claiming hardware interruption semantics: successful stores before a spill fault remain committed; a return fill can place an earlier successful caller word/NaT in the ring before a later read faults, while the return's frame and pointer transition remains uncommitted. Full RSE fault delivery/restart, interrupted operations in other RSC modes, and arbitrary fault recovery remain unimplemented.

The successful pressure and switching paths are qualified in enforced-lazy mode (`RSC.mode=0`). Optional eager transfers and nonzero RSC modes are outside this phase's test coverage. Physical ring behavior for arbitrary rotation/BOF combinations and complete scheduler semantics are also not claimed.

The architectural reference is Intel's [Itanium Architecture Software Developer's Manual, Volume 2, Chapter 6](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf) for physical partitions, mandatory versus eager transfers, RNAT collections, and RSE interruption behavior, together with [Volume 3](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-vol-3-manual.pdf) for instruction semantics and dependencies.

## Checkpoints and validation

Because no serialized CPUState fields changed, checkpoint compatibility/versioning was untouched. `test_vm_checkpoint` remains part of the required regression run; the authentic v5 Linux checkpoint was not changed or migrated. No ISO, EFI image, kernel boot, or QEMU execution was required.

Required executable results:

| Target | Result |
|---|---|
| `test_new_instructions` | Passed |
| `test_ia64_processor_interrupt_block` | Passed |
| `test_ia64_interrupt_controller` | Passed |
| `test_vm_checkpoint` | Passed |
| `test_ia64_single_cpu_context_switch` | Passed A → B → A → B → A; pressure probe and pressure-plus-switch scenario passed |
| `test_ia64_rse_automatic_pressure` | Passed capacity, repeated spill/fill, RNAT, and transfer-fault cases |
| `git diff --check` | Passed |

The configured CMake/Ninja build emitted existing compiler warnings in unrelated files. Its preserved Ninja log reported a premature end-of-file recovery and rebuilt the requested targets; no build tree was removed.

## Closeout

- Source and test implementation commit: `fac6dcc7731605e509a76f4a44ce1226c1946126` (`Implement automatic IA-64 RSE pressure spill and fill`).
- This report is committed separately so it can name the implementation commit.
- Branch remains `IA64`; no push was performed.
- `.phase` remains Phase 8 → Phase 9.
- All pre-existing build and scratch directories remain in place.
