# MIPS64-26: SYNC Memory Ordering and OpenWrt Continuation

## 1. Repository preflight and Git state

- Checkout: `D:\dev\guideXOS_Hypervisor_MIPS64`, branch `MIPS64`.
- Starting HEAD: `134ef1f5931bf79cc02c84590ff9ffaf5d490a74` (the required Phase 25 base).
- Starting upstream divergence: 4 commits ahead, 0 behind `origin/MIPS64`.
- The starting worktree was clean. The `.phase-mips64` marker named MIPS64-25 complete and MIPS64-26 next.
- The IA-64 `.phase` marker was read and left unchanged.
- The Phase 15, 18, 19, and 25 reports and the accepted Phase 25 trace were inspected. `E:\CodexArtifacts\guideXOS_Hypervisor` and documented MIPS64 artifact locations were inspected before replay.
- No branch, worktree, or remote topology change was made; nothing was pushed.

## 2. Kernel and accepted Phase 25 trace identity

The production replay reused the existing OpenWrt 24.10.4, Linux 6.6.110 Malta BE64 kernel and accepted Malta ROM/PROM bootstrap. The kernel is 11,013,776 bytes with SHA-256 `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc`. Its entry is `0xffffffff80917670`; VM RAM is 256 MiB; the PROM command line is `console=ttyS0,38400n8`.

The existing `docs/artifacts/openwrt_malta_be64_dextm_phase25.jsonl` remains unchanged and still hashes to `222593af3cf8e35b2b415190daf3bf1a0b9c00d90eebb03b8d86f6e8752c0c2f`. No kernel, historical trace, or snapshot artifact was overwritten.

## 3. Authenticated SYNC frontier and prestate

The accepted frontier is guest PC `0xffffffff8091eb50`, fetch PA `0x00091eb50`, raw word `0x0000000f`. Independent field extraction gives SPECIAL opcode `0x00`, `rs=0`, `rt=0`, `rd=0`, `stype=0`, and function `0x0f` (`SYNC`). The Phase 25 typed result was `UnsupportedInstruction`; the instruction had not been executed.

The production VM reproduced the frontier after checking the first 256 ROM/PROM trace records byte-for-byte against Phase 25 and re-executing the accepted VM path up to the frontier. Immediately before SYNC:

- CP0 Status `0x00400080`; Cause, EPC, and BadVAddr `0`.
- CP0 Count, VM cycles, and retired instructions: `157780` each.
- Branch delay slot not armed; HI and LO both zero.
- No active CPU reservation (all reservation fields zero); all six hardware interrupt lines false; UART RX FIFO empty.
- The trace records the full 32-GPR prestate and ordered CP0 state.
- Earlier continuation records include LL at PC `0xffffffff8091e94c`, SC at `0xffffffff8091e95c`, and stores at `0xffffffff8091e968` and `0xffffffff8091abb8`. The reservation was no longer active at SYNC.

## 4. Architecture basis

The implementation follows the [MIPS64 Architecture for Programmers, Volume II, Revision 2.50, SYNC entry (printed pp. 347–349)](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf). Stype 0 is the defined synchronization operation: prior synchronizable loads and stores must be globally performed before later synchronizable memory operations proceed. SYNC does not generally order instruction fetch. The manual permits a strongly ordered implementation to give SYNC the effect of a NOP; it does not excuse an implementation that leaves deferred or reordered guest stores outstanding. Nonzero stype values are reserved for future variants and are not assigned guessed behavior here.

The [Volume I, Revision 2.50](https://0x04.net/~mwk/doc/mips/MD00083-2B-MIPS64INT-AFP-02.50.pdf) describes memory access types and CCA distinctions. These references constrain the claims below: the project does not model full coherent caches, coherent DMA, or arbitrary host-parallel SMP.

## 5. Existing memory-ordering contract and barrier action

Guest instruction execution is synchronous and in program order. `Mips64Cpu::executeMemory` performs address translation and calls the physical read/write path inline; a load obtains its bytes before writing its destination, and a store's physical write returns before that instruction returns. `Memory::Read` and `Memory::Write` dispatch to `PhysicalAddressMap` synchronously when the map is enabled. Dense RAM backends apply the access directly. There is no guest write buffer, pending RAM transaction queue, or delayed CPU store that survives instruction retirement.

At the VM level, `VirtualMachine::step` selects one CPU and calls `stepCPU`; `stepCPU` takes the VM snapshot-boundary lock and invokes one CPU step. It increments virtual cycles and retirement accounting only after a successful step. Scheduling may interleave CPUs deterministically, but does not run guest CPUs in parallel. The supported RAM API therefore presents each committed write before the next guest instruction or scheduled CPU instruction observes it.

This is why stype 0 is an explicit `Mips64Op::Sync` with a named execution case, while that case has no separate memory-side action or host atomic fence. The barrier contract is already met by completed inline memory transactions and in-order VM stepping; adding a fence would neither drain a nonexistent queue nor implement missing hardware cache coherence. The implementation does not claim that arbitrary external writes bypassing tracked APIs, simulated cache aliases, or host-parallel agents participate in this ordering model.

Concrete implementation references:

- Decode field extraction and SPECIAL/funct `0x0f` validation: [`Mips64Cpu.cpp`](../src/mips64/Mips64Cpu.cpp#L272) and [`Mips64Cpu.cpp`](../src/mips64/Mips64Cpu.cpp#L352).
- Named stype-0 operation and the reason no drain is needed: [`Mips64Cpu.cpp`](../src/mips64/Mips64Cpu.cpp#L1701).
- Inline guest load/store path: [`Mips64Cpu.cpp`](../src/mips64/Mips64Cpu.cpp#L1943).
- `Memory` dispatch and physical-map synchronous backend call: [`memory.cpp`](../src/memory/memory.cpp#L65), [`PhysicalAddressMap.cpp`](../src/memory/PhysicalAddressMap.cpp#L365), [`PhysicalAddressMap.cpp`](../src/memory/PhysicalAddressMap.cpp#L393).
- VM CPU selection and serialized one-instruction step: [`VirtualMachine.cpp`](../src/vm/VirtualMachine.cpp#L493), [`VirtualMachine.cpp`](../src/vm/VirtualMachine.cpp#L2069), [`VirtualMachine.cpp`](../src/vm/VirtualMachine.cpp#L2125).

## 6. Supported memory types and limits

The modeled Malta profile uses cached, noncoherent CCA 3 for KSEG0 and uncached CCA 2 for KSEG1. The CPU also validates CCA values from mapped address paths; the LL/SC monitor supports its documented cached CCA policy. Dense RAM writes through the guest physical map and the supported `WriteHostRam` API update tracked reservation generations. Direct backing-memory mutation outside those APIs is outside that guarantee.

The physical map invokes an MMIO backend synchronously as one validated transaction. The focused mock device confirms call order when the device itself applies writes immediately. A backend that queues work internally may have additional completion semantics; this phase does not claim universal device completion. No cache arrays, cache invalidation, cycle-accurate write buffers, coherent DMA, or host-parallel memory consistency are simulated.

## 7. Decoder, disassembly, and nonzero stype policy

`Mips64Op::Sync` was appended after existing operations to keep prior enum values stable. The decoded instruction carries `stype` from bits 10:6; this is transient decode data and does not alter serialized CPU state. SYNC validates `rs`, `rt`, and `rd` as zero. Exact word `0x0000000f` decodes as valid stype 0 and disassembles as `sync`.

Nonzero stype remains recognized as the SYNC family, disassembles as `sync N`, and reports typed `UnsupportedInstruction` because Revision 2.50 reserves those future variants and their semantics are not implemented. It is not treated as an unrelated SPECIAL function, and it does not enter the guest Reserved Instruction exception path. Nonzero stype was not speculatively implemented. Malformed fixed register fields classify as `ReservedInstruction`.

EHB, SSNOP, SYNCI, SYSCALL, and BREAK remain distinct. EHB retains its existing operation; SSNOP remains SLL `$zero,$zero,1`; SYNCI does not decode as SYNC. The disassembler and operation-name mapping now include SYNC.

## 8. SYNC architectural effects

The authenticated instruction retires through ordinary dispatch. PC advances from `0xffffffff8091eb50` to `0xffffffff8091eb54`; Count, VM cycles, and retirement each advance once from `157780` to `157781`. GPRs, HI/LO, CP0 other than normal Count accounting, RAM, reservation state/generation, interrupt lines, and UART state remain unchanged. No exception is raised, no interrupt is acknowledged, and no virtual time beyond the ordinary one-step accounting is introduced.

The retirement record confirms all these comparisons from the live production VM. The decoded stype adds no persistent architectural state, so the accepted CPU/full-VM snapshot formats remain unchanged.

## 9. LL/SC reservation integrity

The encoded tests exercise LL/SYNC/SC and LLD/SYNC/SCD. A matching reservation remains valid across SYNC and succeeds without a conflict. SYNC alone does not change the physical map's reservation generations. A tracked host write or a conflicting CPU write still invalidates the reservation and makes SC/SCD fail; a write to another 16-byte granule preserves success. LL/SC and LLD/SCD cross-size mismatches still fail. CPU-local reservation state and separate VM memory maps remain isolated, and invalid snapshot restore leaves live state unchanged.

The existing physical map continues to serialize reservation checks and conditional stores under its reservation monitor; a successful conditional store commits the backend write and advances the tracked generation. SYNC neither clears nor recreates reservations. Existing ERET/reset invalidation behavior is unchanged.

## 10. Encoded architectural and ordering tests

The new [`test_mips64_sync.cpp`](../tests/test_mips64_sync.cpp) CTest target covers exact encoding, SPECIAL/function and stype fields, malformed fixed fields, future stype classification, disassembly, both instruction endian modes, ordinary retirement, no unintended register/CP0/HI/LO/memory/interrupt changes, and taken/not-taken branch delay slots including CPU snapshot replay.

Memory scenarios use encoded guest instructions and production `Memory`/`PhysicalAddressMap` paths:

- Store/SYNC/load observes the committed value in both endian modes.
- DATA/SYNC/FLAG observes DATA committed before FLAG.
- Two CPUs use an explicit deterministic CPU0 then CPU1 instruction interleaving; CPU1 observes FLAG and then the preceding DATA. This demonstrates the VM's serialized model, not arbitrary host-thread coherence.
- LL/SYNC/SC and LLD/SYNC/SCD, tracked conflicts, different granules, cross-size rules, snapshot replay, malformed snapshot rejection, and VM isolation are checked.
- An immediate MMIO mock observes write/SYNC/write order. This proves synchronous dispatch for that mock only.

CTest registration is in [`CMakeLists.txt`](../CMakeLists.txt#L1321). The focused executable reported **279 passed, 0 failed**.

## 11. Snapshot and deterministic replay

No snapshot version or persistent CPU layout changed. Formats remain full VM `GXM64SNP` v3, CPU v6/1,001 bytes, memory v2, UART v4, and IRQ routing v1.

Synthetic replay captures/restores around SYNC-only and store/SYNC/load sequences, active LL/SYNC/SC reservation state, and a SYNC branch delay slot; replays reproduce CPU state, memory bytes, reservation generations, PC, and counters. An invalid physical-memory snapshot is rejected without changing live CPU or virtual time.

The production test captures a full VM snapshot immediately before authentic SYNC, retires it, and continues to the next frontier; it then restores and repeats the SYNC and continuation. The replay matches CPU, guest RAM, reservation state, devices/UART, scheduler state, counters, and stop frontier. Console generation bookkeeping is excluded from comparison; there was no console output.

## 12. Authentic Linux continuation and trace

The one-million-instruction continuation budget was not exhausted. The production replay retired **157,787 total instructions**, of which **157,770** were since Linux entry. It retired **7 instructions beyond the former unsupported SYNC frontier** (SYNC plus six more), including **6 instructions after SYNC**. The bounded stop took 157,788 attempts: the next frontier predecode did not retire.

The new trace `docs/artifacts/openwrt_malta_be64_sync_phase26.jsonl` has 270 valid JSONL records and SHA-256 `5b7627802f8365e080e9edff2565818a8696a63b5bd59791452c435625645397`. It records the production environment and verified Phase 25 prefix, a compact gap marker for the deterministically re-executed interval, the pre-SYNC memory activity, complete SYNC prestate and retirement observations, invalid snapshot rejection, six post-SYNC retirements, and the next frontier. The gap is trace compression; the VM executed the interval rather than skipping guest state.

The next genuine frontier is guest PC `0xffffffff8091abc4`, fetch PA `0x00091abc4`, word `0x2a220000`, with opcode `0x0a`, `rs=17`, `rt=2`, immediate `0`. Volume II defines this as signed-immediate `SLTI`; the current decoder reports it as a valid opcode outside the implemented subset. See the [Volume II SLTI definition (printed p. 337)](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf). It was not executed in Phase 26.

The terminal state has Count/cycles/retirement `157787`; Status remains `0x00400080`; Cause/EPC/BadVAddr are zero; delay slot is clear; there are no valid TLB entries or active reservations. There are no architectural exceptions, UART TX/RX bytes, or console bytes. Linux boot completion is not claimed.

## 13. Regression totals and production linkage

- Pre-change inherited MIPS64 baseline: **53/53 passed**.
- Final MIPS64 CTest: **54/54 passed**.
- Selected cross-ISA regressions (`BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`, `RiscVDecoderSmokeTest`): **6/6 passed**.
- Combined final selected run: **60/60 passed**.
- The accepted Phase 25 report records 424,163 MIPS64 assertions; adding this phase's 279 focused checks gives **424,442 reported MIPS64 checks**.
- Rebuilt the focused MIPS64 target and production `ia64emu.exe`. Symbol/string inspection confirmed linked `decodeInstruction`, `Mips64Cpu::executeDecoded`, `Mips64Cpu::disassemble`, `Mips64Cpu::opName`, the `sync` mnemonic, and the reserved-stype diagnostic.

The unrelated historical IA-64 NaT assertion and unavailable legacy test executables noted by prior reports remain outside this change. No IA-64 source or marker was modified.

## 14. Recommended MIPS64-27 scope

Implement the authentic next frontier, signed-immediate `SLTI` (`opcode 0x0a`), beginning with exact word `0x2a220000` (`slti $2,$17,0`). Qualify sign-extension of the 16-bit immediate, signed 64-bit comparison, destination `$zero` handling, both endian modes, branch delay slots, retirement/counter effects, and authentic OpenWrt continuation. Preserve the current unsupported-instruction stop until that separate implementation is qualified.

## 15. Phase classification and commit

**Outcome A — Complete.** The authentic frontier was reproduced and decoded, the source-backed strong-ordering contract was qualified, SYNC retired with the required preserved state, encoded ordering and LL/SC scenarios passed, both endian modes and delay slots passed, deterministic snapshot replay passed, the production VM made Linux forward progress to a valid next frontier, all requested regressions and production linkage passed, and the trace/report were produced without changing the accepted kernel or Phase 25 artifact.

The work is committed locally on `MIPS64`; the `.phase-mips64` marker now records MIPS64-26 complete and MIPS64-27 next. The separate IA-64 `.phase` marker is unchanged. The final commit and Git status are recorded with the handoff.
