# IA64-POST-MADT-9R8 — Architectural RSE Fault Delivery and Restart

## Outcome

**Outcome A — RSE interruption and restart verified for automatic `alloc` spills, explicit `flushrs` spills, and `br.ret` target fills.** The phase gate remains at `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`. R8 qualifies this bounded synthetic subphase; it does not qualify the authentic Linux scheduler or close Phase 9.

## Repository state

- Branch: `IA64`.
- Starting HEAD: `e72137cb63674f88d46468160cdbb466707efea5` (`IA64: verify R7 checkpoint continuation`).
- Ending HEAD: local commit `IA64: add restartable RSE fault delivery` (one commit added to the starting branch).
- `.phase`: unchanged at the 8 → 9 gate.
- Build tree: `build-9r8/`.
- Existing untracked build trees and artifacts were preserved: `build-7r1/`, `build-9r2/` through `build-9r5/`, and `build-9r7/`.
- Upstream comparison: `origin/IA64...HEAD` was 0 behind / 5 ahead at preflight; the R8 commit makes it 0 behind / 6 ahead.
- Remote: no push performed.

## Existing fault path and fault classification

RSE backing-store reads and writes use the IA-64 address translator. The translator raises `IA64TranslationFault` for typed guest translation conditions such as a data page-not-present fault or access-right fault. The instruction execution path catches that type, saves the interruption registers, selects the translator-provided vector, and transfers to the guest handler.

The RSE transfer helpers now let this typed exception escape after recording completed transfers. Generic `IMemory` exceptions, invalid frame sizes, inconsistent RSE partitions, invalid `RSC` modes, and malformed host addresses remain on the existing host error path. They are not relabeled as guest page faults. Nested-TLB-miss register preservation follows the existing special path and is not exercised by this regression.

## Architectural fault matrix

Vector selection follows the translation-fault class. The tested data page-not-present case uses vector offset `0x5000` (DPNP); another translation class selects its own translator-provided offset. The manuals define RSE faults as synchronous and distinguish the instruction that caused a mandatory transfer from a fill fault attributed to the target instruction. `ISR.rs` marks a mandatory RSE transfer, `ISR.ir` identifies an incomplete target frame, and the ordinary access bit records read or write. When interruption collection was enabled, entry clears `CR.IFS.v`; `rfi` therefore preserves the already selected current frame. The interrupted `PSR` is saved in `CR.IPSR`, and `rfi` restores it with the saved `CR.IIP`.

| Operation | Required fault attribution | Expected status | R8 implementation and coverage |
|---|---|---|---|
| `alloc` mandatory spill | Issuing bundle and slot | `ISR.rs=1`, write bit set, `ISR.ir=0`; the data-fault vector | Implemented and tested at the first store and after three stores across an RNAT collection slot and region boundary. The guest retries `alloc`. |
| `flushrs` mandatory spill | Issuing bundle and slot | `ISR.rs=1`, write bit set, `ISR.ir=0`; the data-fault vector | Implemented and tested after three stores, across an RNAT collection slot and region boundary; the guest handler repairs the mapping and `flushrs` resumes. |
| `loadrs` mandatory fill | Issuing bundle and slot | `ISR.rs=1`, read bit set, `ISR.ir=0`; the data-fault vector | Delivery metadata is wired through the common typed-fault path. Restart after partial loads is not implemented or qualified. |
| `br.ret` mandatory frame fill | Target bundle and target RI | `ISR.rs=1`, read bit set, `ISR.ir=1`; the data-fault vector | Implemented and tested with no completed load and after three completed loads. An incomplete target frame remains visible until the pending fill completes. |
| `rfi` target fill | Target bundle and target RI | Mandatory-fill status, including `ISR.ir=1`; the data-fault vector | Not implemented or tested. The current `rfi` path restores its saved state but does not initiate a separate RSE target fill. |

For the exercised ordinary DPNP faults, entry saves `CR.IIP`, `CR.IPSR`, `CR.ISR`, `CR.IFA`, `CR.ITIR`, and `CR.IHA`; clears `CR.IFS.v` when the interrupted `PSR.IC` is set; clears interruption collection, interrupts, RI, and bank selection for the handler; and vectors to `IVA + 0x5000`. For the tested target fill, saved `IIP` is the target bundle and saved RI is zero. `AR.RSC` is unchanged. Spill and fill progress is retained in the existing BSPSTORE, `AR.RNAT`, RSE load/store cursor, and per-register partition state.

## Synthetic guest and instruction encodings

`test_ia64_rse_fault_restart` creates a persistent one-CPU VM with 1 MiB of guest RAM, real encoded bundles, the production IA-64 decoder and execution path, and the production DTLB translator. It initializes a legal 96-entry physical register stack and a 64-register caller frame with distinguishable values and NaT bits. The backing store begins at either `0xAFE0` (the RNAT-boundary case) or `0xB000` (the first-transfer case). The data page at `0xA000` is mapped; the `0xB000` page is initially a valid but not-present translation.

The guest executes a direct `br.call`, `alloc r40=ar.pfs,64,64`, translation invalidation/insertion, restores `ar.pfs` from `r40`, and executes `br.ret b0`. The caller continuation restores the test TTE register before the final state comparison. The test's generated slot encodings are:

| Instruction | Bundle address | 41-bit slot value |
|---|---:|---:|
| `flushrs` | `0x10000` | `0x60000000` |
| `alloc r40=ar.pfs,64,64` | `0x11000` | `0x2C04080A00` |
| `ptc.l r19,r20` | `0x11010` and `0x35000` | `0x2049328000` |
| `itc.d r18` | `0x11020` | `0x2170024000` |
| `mov ar.pfs=r40` | `0x11030` | `0x154050000` |
| `br.ret b0` | `0x11040` | `0x2020000100` |
| Guest handler `itc.d r16` | `0x35010` | `0x2170020000` |
| Guest handler `rfi` | `0x35020` | `0x40000000` |
| Caller continuation `mov r18=r16` | `0x10010` | `0x10070020480` |

The page-fault handler runs guest `ptc.l r19,r20` to purge the page named by `CR.IFA`, then guest `itc.d r16` using the interruption-saved `IFA` and `ITIR`, and finally guest `rfi`. It uses static registers, preserving the interrupted stacked frame and `RNAT`. The fill scenario's guest `ptc.l r19,r20` followed by `itc.d r18` installs a not-present entry immediately before `br.ret`, making the fill fault recoverable by the same guest handler. Before `br.ret`, the guest restores `ar.pfs` with `mov ar.pfs=r40`, as required by the IA-64 calling convention. The `flushrs` fixture starts with 64 dirty stacked registers, faults on the fourth data store after the RNAT slot at `0xAFF8`, then uses the same guest mapping repair and `rfi` path.

## Fault and restart observations

With a backing store at `0xAFE0`, automatic `alloc` spilling completes data stores at `0xAFE0`, `0xAFE8`, and `0xAFF0`, then writes the RNAT collection at `0xAFF8`. Its next data transfer at `IFA=0xB000` faults. The recorded frontier is `BSPSTORE=0xB000`, store cursor `3`, and the successful prefix is clean while remaining caller registers stay dirty. `alloc` has not committed the callee frame: `CFM` remains the empty call frame and `PFS` retains the caller's 64/64 frame. The guest maps the page and returns; the original `alloc` retries and completes the remaining spill without repeating the successful prefix.

With the backing store at `0xB000`, the same mandatory store faults before its first register transfer. `BSPSTORE` remains at `0xB000` and the store cursor remains zero. Mapping and retry complete the spill.

The test then makes `0xB000` not present again and returns. In the boundary case, `br.ret` has already filled three caller registers from `0xAFE0`, `0xAFE8`, and `0xAFF0` before faulting at `0xB000`; its load pointer is `0xB000` and load cursor is `3`. In the first-transfer case, both remain at the start with cursor zero. In each case, the target caller `CFM` and rewound `BSP` are visible, `BSPSTORE > BSP`, and `ISR.ir` is set. The handler maps the page and returns through `rfi`; before the target bundle executes, the plugin completes the saved fill from the load pointer and then resumes at the target. The completed prefix is not loaded a second time.

The standalone `flushrs` fixture faults after stores at `0xAFE0`, `0xAFE8`, and `0xAFF0` and after the RNAT collection slot at `0xAFF8`, with `BSPSTORE=0xB000`, store cursor `3`, and the three completed registers clean. The guest repairs the translation and returns; the resumed instruction stores the pending registers. Its final RAM, physical GR/NaT state, RSE cursors, and partitions match the fault-free run.

The boundary case checks NaT restoration on both sides of the RNAT slot: a caller NaT is recovered from the backing RNAT collection and another from the live `AR.RNAT` collection. The first-transfer and boundary `alloc`/`br.ret` runs each compare the complete guest RAM image, all physical general-register values and NaT bits, CFM/PFS/RSC/BSP/BSPSTORE/RNAT, and RSE cursors and partitions against a fault-free reference run. Each run enters the real guest vector handler twice (once for spill and once for return fill). The separate `flushrs` run compares the same final architectural state.

## Checkpoint and regression results

The restart frontier uses RSE fields already included in the checkpoint representation (`BSPSTORE`, `bspLoad`, load/store cursors, `RNAT`, and partitions); no checkpoint format version or new field was added. The R7 continuation target validates checkpoint continuation at its supported stable boundary. A checkpoint captured during an in-flight RSE transfer is not qualified by this phase.

| Regression target | Result |
|---|---|
| `test_ia64_rse_fault_restart` | Passed: first-transfer spill/fill, partial RNAT-boundary spill/fill, and partial `flushrs` restart |
| `test_new_instructions` | Passed |
| `test_ia64_processor_interrupt_block` | Passed (standalone executable) |
| `test_ia64_interrupt_controller` | Passed (standalone executable) |
| `test_vm_checkpoint` | Passed |
| `test_ia64_single_cpu_context_switch` | Passed (standalone executable) |
| `test_ia64_rse_automatic_pressure` | Passed |
| `test_ia64_checkpoint_context_continuation` | Passed |
| `git diff --check` | Passed |

## Limitations and next phase

This result covers recoverable typed data-translation faults in automatic `alloc` spill, explicit `flushrs` spill, and `br.ret` target fill under enforced-lazy RSE operation. `loadrs` partial-load restart, `rfi`-initiated target fill, nested translation interruptions, other RSC modes, and checkpointing during a transfer remain outside the verified scope. Generic host-memory and emulator failures continue to stop through the existing error path. No ISO, EFI, Linux, QEMU, or historical checkpoint artifact was required.

Recommended next development phase: add guest-level restart qualification for partial `loadrs` faults with nonzero `RSC.loadrs`, then investigate `rfi`-initiated fill only if the execution model exposes that architecture path. Continue to preserve the Phase 9 gate until authentic Linux scheduler qualification succeeds.

## Local commit and push status

- Local commit: `IA64: add restartable RSE fault delivery`.
- Ending HEAD: contains the local R8 commit atop the preflight `IA64` HEAD; upstream divergence is 0 behind / 6 ahead.
- Push: not performed.
- Worktree: implementation, test, and this document are committed locally; the existing untracked build artifacts and the separate `build-9r8/` outputs remain present.

## Architectural sources

- Intel, *Itanium Architecture Software Developer's Manual, Revision 2.3, Volume 2: System Architecture*, §§3.3.5.2 and 5, §§6.6–6.8, and §6.11: [PDF](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf).
- Intel, *Itanium Architecture Software Developer's Manual, Revision 2.3, Volume 3: Instruction Set Reference*, `alloc`, `flushrs`, `loadrs`, `br.ret`, and `rfi`: [PDF](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-vol-3-manual.pdf).
