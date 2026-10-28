# PPC64 Phase 5 — Atomic Memory Operations and Synchronization

Branch: `PPC64`. Starting commit: `8e5ca0f7ad5368349f08a5ba0ddc676baa035083`.
The phase marker advances only after the Phase 5 and regression suites pass.
The shared IA-64 `.phase` file is unchanged.

Architectural reference: [OpenPOWER Power ISA v3.1C](https://openpower.foundation/specifications/isa/),
Books I and II. This report describes a bounded single-threaded interpreter
profile, not full Power ISA memory consistency or host-parallel SMP.

## 1. Instruction coverage and encodings

All encodings are big-endian 32-bit words decoded and executed by the
production `PPC64ISAPlugin`.

| Instruction | Encoding fields | Supported behavior |
|---|---|---|
| `lwarx RT,RA,RB` | opcode 31, XO 20, EH in bit 31 | Load 4 bytes, zero-extend, reserve the real storage location; EH=0/1 is accepted as a hint and ignored. |
| `ldarx RT,RA,RB` | opcode 31, XO 84, EH in bit 31 | Load 8 bytes and reserve the real storage location; EH=0/1 is accepted as a hint and ignored. |
| `stwcx. RS,RA,RB` | opcode 31, XO 150, Rc=1 | Conditional 4-byte store. Rc=0 is an invalid form. |
| `stdcx. RS,RA,RB` | opcode 31, XO 214, Rc=1 | Conditional 8-byte store. Rc=0 is an invalid form. |
| `sync` | `0x7C0004AC` | Supported heavyweight synchronization form (L=0). |
| `lwsync` | `0x7C2004AC` | Supported lightweight synchronization form (L=1). |
| `isync` | `0x4C00012C` | Supported instruction-context synchronization form. |
| `eieio` | `0x7C0006AC` | Supported in-order I/O instruction form. |

For the atomic X-forms, the word is assembled as
`(31<<26) | (RT<<21) | (RA<<16) | (RB<<11) | (XO<<1) | bit31`.
The accepted anchor words are `lwarx r3,r4,r5 = 0x7C642828`,
`ldarx r3,r4,r5 = 0x7C6428A8`, `stwcx. r3,r4,r5 = 0x7C64292D`, and
`stdcx. r3,r4,r5 = 0x7C6429AD`.

Indexed effective addresses use `(RA == 0 ? 0 : GPR[RA]) + GPR[RB]`, with
unsigned 64-bit wrap behavior. GPR0 is still read normally when it is RB.
`lwarx` requires 4-byte alignment; `ldarx` requires 8-byte alignment. Data is
big-endian. Invalid/reserved forms are rejected. Other reservation widths,
`ptesync`, persistent-storage synchronization forms, and reserved encodings
are deterministic emulator limitations, not NOPs.

## 2. Reservation record and granule

Each `PPC64ISAPlugin` owns one runtime `ReservationRecord` containing its
owning memory hub, exact real address, access width, granule, interference
epoch, and valid bit. A second load-reserve replaces the first. A conditional
store consumes the record regardless of success. The record is not a guest
SPR and is not part of CPU serialization.

The modeled reservation granule is 128 bytes. This is an implementation
choice for this virtual CPU, not a size required for every Power processor.
Writes overlapping any byte in that granule invalidate the reservation;
writes in other granules leave it valid. Conditional stores also require an
exact address and width match, which permits conservative failure for a
nonmatching operation.

PPC64 Phase 4 supports only real-address execution with identity mappings.
Memory range and permission validation rejects non-identity translations, so
the effective address is the real backing-storage address in this phase.
Mapping or permission generation changes conservatively invalidate all
reservations before the next reservation operation.

## 3. Shared-memory interference and supported writers

`Memory` owns one `MemoryReservationHub`; CPUs sharing that `Memory` instance
share invalidation state, while different `Memory` instances remain isolated.
The hub observes successful `Memory::Write`, `loadBuffer`, typed
`Memory::write<T>`, `Clear`, restore from `MemorySnapshot`, and successful
conditional stores. Device registration changes and MMU map/permission
changes invalidate reservations conservatively. Writes through these paths
that overlap a reservation granule cannot produce a stale success.

An `IMemory` backend must return a hub only when it can observe all relevant
mutations. The default returns no hub, so reserve/store-conditional
instructions fail deterministically on untracked backends. `Memory` rejects
reservation targets that overlap any registered device range, including a
partial overlap. Device state may change outside the memory write path, so no
atomic support is claimed for MMIO.

The supported observation boundary is the public memory API and the
registered-device interface. Mutation through `const_cast` of `GetRawData()`,
out-of-band device state changes, or custom callbacks that re-enter and
mutate guest memory are not tracked as coherent guest writes. The MMU hook
interface has no atomic/reentrancy contract; hooks used with atomic programs
must remain observational. `Memory` itself does not promise thread safety.

## 4. Conditional-store behavior and flags

Before deciding success, `stwcx.`/`stdcx.` validate alignment, address range,
translation profile, and write permissions across the access. A protection or
unmapped fault follows Phase 4 DSI behavior even when no reservation matches;
it does not become an ordinary failed conditional store. Once valid, the
interpreter checks the reservation and performs a successful write through
one `Memory::Write` call. A failed conditional store does not call the write
path and leaves memory unchanged.

Both normal outcomes clear the local reservation and set CR0 to
`{LT=0, GT=0, EQ=success, SO=XER.SO}`. CR fields 1–7 and XER are preserved.
No unrelated GPR, LR, or CTR is changed. Faulting conditional stores do not
fabricate a CR0 failure result.

## 5. Commit boundary and concurrency limits

Execution is deterministic and single-threaded. No scheduler yield occurs
between reservation validation and the single memory write inside one
instruction step; all supported guest CPU instances are interleaved only
between complete `step()` calls. The hub has no host mutex, and this phase
does not support concurrent host execution of guest CPUs or external writers.
Passing the interleaving tests therefore establishes deterministic
single-thread behavior only, not thread-safe SMP atomicity.

## 6. Reservation lifetime

- A successful `lwarx`/`ldarx` establishes or replaces the reservation only
  after all checks and the load succeed. A failed reserve does not alter the
  destination register or leave a reservation.
- An ordinary load does not explicitly clear a reservation in this model.
- A conflicting ordinary store, store by another CPU, or successful
  conditional store clears every overlapping record. An out-of-granule write
  does not.
- Any executed conditional store consumes the current reservation, including
  address/width mismatch or normal failure.
- Guest exception entry, `reset`, `setPC`, `setState`, and state
  deserialization clear the reservation. This is conservative across
  exception handlers and control-context replacement.
- Reservation state is absent from both snapshot formats. Restoring either
  CPU state or `MemorySnapshot` cannot revive a pre-restore reservation.

## 7. Synchronization semantics and limits

The interpreter executes instructions in stream order and completes each RAM
read/write synchronously before the next instruction step. It has no store
buffer or speculative/out-of-order execution. On ordinary RAM, this gives
`sync` and `lwsync` the required storage-order effect for this sequential
profile; the two forms have no distinct performance or weaker-order behavior
here.

`isync` completes prior instruction effects, drops the cached decode, and the
next step fetches its instruction again. The interpreter has no separate
instruction cache to invalidate. `eieio` advances in stream order and invokes
device handlers in that same order. The device interface does not specify
flush or completion semantics, so this implementation makes no stronger
real-MMIO ordering claim. No host hardware fence is used, and full
multiprocessor memory ordering is out of scope.

## 8. Exception integration

Legacy mode returns the Phase 4 `EXCEPTION` result with the faulting NIP
held. Guest-delivery mode uses Phase 4 Alignment and DSI vectors, DAR/DSISR,
SRR0/SRR1, and `rfid`. Misaligned reserves use the Alignment vector with
DAR=EA and DSISR=0. Protection and unmapped faults on reserve/conditional
store operations use DSI with the existing Phase 4 cause bits. Destination,
memory, and CR0 remain unchanged on faults. All guest exception entries drop
the reservation so a handler cannot consume a stale record after recovery.

## 9. Serialization and snapshots

The architectural formats remain unchanged: v1 is 304 bytes and v2 is 332
bytes, with Phase 4 SRR0/SRR1/DAR/DSISR support preserved. Runtime reservation
state is absent from both. `deserialize_state`, `deserialize_state_v2`, and
`Memory::RestoreSnapshot` invalidate any live record, so a state restore
cannot authorize a conditional store using an older reservation.

## 10. Executable programs and tests

`tests/test_ppc64_phase5.cpp` contains 496 checks and executes genuine
PowerPC instruction words through the production decoder and executor:

- **A — 32-bit atomic increment:** five `lwarx`/`stwcx.` increments from
  `0x11223344` to `0x11223349`; retry and completion branches, count, CR0,
  step count, memory, and final NIP are asserted.
- **B — 64-bit atomic increment:** four `ldarx`/`stdcx.` increments from
  `0x0000000100000000` to `0x0000000100000004`; full-width state and
  big-endian memory are asserted.
- **C — interference and retry:** CPU B's ordinary `stw` changes the shared
  counter after CPU A reserves it. CPU A's first conditional store fails,
  branches to retry, then succeeds; final value is 21. Equal numeric
  addresses in separate `Memory` instances remain isolated.
- **D — reservation boundaries:** tests second-reserve replacement,
  same-granule invalidation, out-of-granule preservation, ordinary loads,
  writes by the reserving CPU and another CPU, width mismatch, typed and bulk
  writes, clear, MMU generation changes, and memory restore.
- **E — exception interaction:** legacy alignment behavior, guest Alignment
  delivery and retry, protected ordinary stores, protected `stwcx.` DSI,
  preserved memory/CR, `rfid` recovery, and rejection of stale reservation
  reuse are asserted.
- **F — synchronization protocol:** encoded `sync`, `lwarx`, `cmpwi`,
  `stwcx.`, retry branches, `isync`, `lwsync`, ordinary release store,
  `eieio`, and final state are executed in the sequential interpreter.

Additional tests cover atomic decoding and reserved forms, RA=0 behavior,
alignment, memory faults, CR0/XER preservation, each synchronization
instruction's register preservation, untracked-memory rejection, device
range overlap, state replacement, reset, and v1/v2 snapshot safety.

## 11. Regression results

Phase 5: **496/496 checks passed**. Earlier PPC64 phase tests are kept as
separate CTest targets and passed alongside the runnable shared-memory and
IA-64 CTest regressions. The selected CTest run passed **11/11** targets:
`BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`,
`VMCheckpointTests`, `RiscVDecoderSmokeTest`, and PPC64 phases 1–5.
Standalone `test_ia64_interrupt_controller` and
`test_ia64_processor_interrupt_block` both passed. The six CTest targets
documented in prior phase reports as pre-existing MinGW `ws2_32` link
failures (`SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`,
`VMIsolationTests`, `ELFValidationTests`) were not part of this runnable
selection. The legacy `test_isa_plugin` NaT serialization abort remains a
documented unrelated issue and was not modified.

| Suite | Checks / result |
|---|---:|
| PPC64-1 | 148/148 |
| PPC64-2 | 399/399 |
| PPC64-3 | 535/535 |
| PPC64-4 | 378/378 |
| PPC64-5 | 496/496 |

## 12. Remaining supported-path limits and next work

The executor is a standalone `PPC64ISAPlugin`, not a PPC64 scheduler inside
`VirtualMachine`. Host-parallel CPUs, radix/hash translation, non-identity
real-address mapping, unsupported reservation widths, MMIO atomics,
asynchronous device completion, and full POWER memory consistency remain
unsupported. Custom `IMemory` backends need a coherent reservation hub before
they can run atomics. A future SMP phase will need a synchronized memory
transaction API and a device completion/order contract.
