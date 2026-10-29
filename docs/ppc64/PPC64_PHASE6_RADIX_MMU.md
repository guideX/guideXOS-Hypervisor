# PPC64 Phase 6: POWER9 Radix MMU

## Profile and scope

Phase 6 implements one bounded POWER9 Book III-S process-scoped radix profile:

- 52-bit effective address payload, with guest quadrant 0 selecting `PIDR` and quadrant 3 selecting PID 0. Quadrants 1 and 2 and other segment encodings are rejected.
- A process table contains 16-byte entries. `PRTS` values 5 through 31 are accepted; the table size is `1 << (PRTS + 12)` bytes. The first big-endian doubleword is the process radix root descriptor.
- The root descriptor must select a 52-bit radix tree (`RTS + 31 == 52`) with root `NLS=13`. The supported 4 KiB path uses `NLS=9` for the next three tables, giving a bounded four-level walk with a maximum depth of four.
- Only normal-memory, 4 KiB leaf PTEs are implemented. Other valid page sizes, I/O storage attributes, hash translation, and partition-scoped radix translation are outside this profile.
- The supported leaf authorities are EAA R, RW, X, and PRIV. RW grants both reads and writes; the architecture does not define a separate write-only leaf authority in this profile.
- POWER9 R/C state must already be set for the requested access: R is required for fetch/read, and R plus C for write. This implementation reports the storage fault when required state is absent and does not mutate guest page-table bytes.

The address path is:

```text
effective address -> guest process radix tree -> guest real address
                  -> explicitly configured flat guest-real/backing map
                  -> Memory backing RAM
```

The final flat map is an emulator test-environment contract. It is not a PATE, LPID, PAPR partition table, or implementation of the architectural partition-scoped translation stage. Real pSeries setup would supply the process table through PAPR process-table registration and provide a real partition context; those interfaces are not implemented here.

The profile follows the Power ISA v3.1C Book III-S formats and is cross-checked against QEMU's POWER9 radix walker and Linux's PPC64 radix/KVM implementation. See the [OpenPOWER ISA specifications](https://openpower.foundation/specifications/isa/), [QEMU POWER radix walker](https://qemu.googlesource.com/qemu/%2B/fc524567087c2537b5103cdfc1d41e4f442892b6/target/ppc/mmu-radix64.c), and [Linux PPC64 KVM radix walker](https://android.googlesource.com/kernel/common/%2B/95e6ba5133163f8241c9ea2439369cec0452fec6/arch/powerpc/kvm/book3s_64_mmu_radix.c).

## Configuration contract

`PPC64RadixConfiguration` is hypervisor-owned state, separate from guest registers. It contains the process-table guest-real base and PRTS value, plus a flat mapping from a guest-real interval to an equally sized offset in the `Memory` backing store. Configuration validation checks PRTS and alignment, overflow, the full process-table interval, and host RAM bounds. The process table, radix directories, leaves, and translated data must all fall inside the flat interval and avoid device-claimed backing ranges.

The initial implementation intentionally requires the repository's concrete `Memory` backend because `IMemory` does not define a guest-real/backing access contract. Unsupported backends and MMIO accesses become emulator-functionality failures; they are not reported as guest translation exceptions. The explicit mapping keeps effective, guest-real, and host-backing addresses distinct.

`MSR[IR]` and `MSR[DR]` act independently. When set, instruction fetch or data access uses the configured radix context. When clear and a flat guest-real map is configured, real-address accesses use that map to reach backing RAM; this includes exception-vector fetches and data accesses during handlers. Without a configured map, the existing Phase 4 real-address path remains available. Setting relocation without a valid radix configuration remains an unsupported-state failure. HV, IP, and LE remain unsupported; other MSR bits are preserved. Guest exception entry clears IR/DR and uses real-address vectors. `rfid` validates and restores the saved relocation mode.

## Guest table format and walk

Process-table entries are indexed by `PID * 16`. A PID outside the configured table is a missing process entry. A zero root descriptor is an invalid radix configuration. The root descriptor uses the POWER process-root RPDB/RPDS and split RTS fields. For the supported tree, the root base is 64 KiB aligned; each subsequent table base is aligned to the table size implied by its NLS (4 KiB for NLS 9).

Directory and leaf entries are read as big-endian 64-bit words from guest-backed memory. The walker checks the valid and leaf bits, reserved fields, expected NLS at each level, table bounds, and address arithmetic. Directory entries carry the next base and NLS. The 4 KiB leaf contributes the guest-real page number; the effective address's low 12 bits provide the page offset. Translation returns the original effective address, guest-real address, host-backing address, page size, effective permissions, privilege constraint, effective PID, and a configuration-generation/PID context identifier.

The implemented leaf fields are `VALID`, `LEAF`, `RPN`, `R`, `C`, `ATT`, and EAA. Only `ATT=normal` is accepted. Reserved bits and unsupported tree shapes are detected. A valid larger-page leaf is reported as unsupported emulator functionality because only the 4 KiB leaf profile has been implemented and tested.

EAA R allows reads, EAA RW allows reads and writes, and EAA X allows instruction fetch. EAA PRIV denies access in problem state; supervisor state still observes the EAA access bits. A denied fetch becomes an ISI protection fault, and a denied data access becomes a DSI protection fault. POWER9's R/C rules are checked after access permissions; no guest page-table bits are changed by hardware-like auto-update.

## Faults and exceptions

Structured translation outcomes distinguish out-of-table PIDs, missing/invalid radix entries, invalid table configuration, protection failures, absent R/C state, unsupported address quadrants, guest-real page-table access failures, and unsupported backend/profile functionality. Recognized storage faults enter Phase 4's ISI/DSI vectors when guest delivery is enabled. Fetch faults retain the faulting NIP and effective address; DSI records the effective address in DAR and sets the supported DSISR cause bits. Missing leaves use the no-translation cause, EAA denial uses the protection cause, bad root/table configuration uses the radix bad-configuration cause, and absent R/C state uses the atomic R/C cause. Unsupported addresses, host-memory failures, and unsupported backend operations remain terminal emulator exceptions.

The vector runs with relocation disabled. Tests repair a leaf while in the real-mode handler, execute `rfid`, and verify the original instruction is retried. The legacy Phase 4 exception-result mode remains available when guest delivery is disabled.

## Data transactions and atomics

Translated loads and stores translate every page segment touched by the full effective range and validate each host-backing segment before committing register or memory effects. Cross-page stores use `Memory::WriteGuestRealTransaction`, which validates all RAM segments before writing any bytes and notifies the existing reservation hub after commit. MMIO is refused because the backend cannot promise transactional behavior for multi-segment device operations. This is a single-threaded emulator contract; it does not claim host-parallel atomicity.

`lwarx`, `ldarx`, `stwcx.`, and `stdcx.` translate their effective addresses before storage access. Reservations use the backing address reached through the flat map, so two effective aliases of one guest-real word share the same reservation identity. Stores through aliases notify the same reservation hub. Conditional stores retranslate and recheck permissions before comparing the reservation, and the store commits only after all translated segments are valid. Existing granule and CR0 behavior is preserved.

## Coherency, checkpointing, and lifecycle

There is no translation cache. Each translated fetch or data operation reads the current process table and radix entries, so PTE removal, replacement, and permission changes take effect on the next access without `tlbie` support. The tradeoff is a page-table read on each translation. PIDR changes, relocation changes, configuration replacement, state replacement, and `rfid` clear the current CPU reservation. The `Memory` reservation hub also invalidates reservations on backing writes, `Clear`, snapshot restore, and legacy MMU mapping-generation changes.

PPC64 checkpoint compatibility is:

| Format | Size | Behavior |
| --- | ---: | --- |
| v1 | 304 bytes | Preserved; restored PIDR is zero |
| v2 | 332 bytes | Preserved; restored PIDR is zero |
| v3 | 340 bytes | v2 state plus an eight-byte big-endian PIDR field; values above 32 bits are rejected |

The automatic serializer retains v1/v2 output while PIDR is zero and emits v3 when it is nonzero. Unknown versions are rejected. Restore clears live reservations and decode state. Radix configuration is hypervisor-owned and is not serialized with CPU state; the host must reapply or retain a valid configuration when constructing/restoring a CPU. Because no translations are cached, restore cannot revive a stale permission result.

## Executable programs and verification

`tests/test_ppc64_phase6.cpp` constructs real big-endian process and radix tables in `Memory`, then executes production PPC64 machine instructions. It checks expected effective-to-real-to-backing addresses directly against the constructed tables.

Instruction fetch cannot straddle a 4 KiB page boundary in this fixed-width profile: PPC64 instructions are four bytes and fetch addresses must be four-byte aligned, so the last valid instruction position in a page is `0xFFC`.

| Program | Coverage |
| --- | --- |
| A — Non-identity execution | Fetches and executes `addi` from a guest-real page distinct from its effective address and host backing offset. |
| B — Translated data | Stores through one effective page and loads through an alias to the same guest-real word; also verifies IR-only and DR-only operation. |
| C — Protection exception | A store to a read-only page enters DSI; DAR/DSISR/SRR0 are checked, the leaf is repaired, `rfid` returns, and the store retries. |
| D — Instruction exception | Non-executable and absent code leaves enter ISI; SRR0/SRR1 are checked, mapping is repaired, and execution resumes through `rfid`. |
| E — Cross-page access | `ld`/`std` span two noncontiguous guest-real pages; removing the second leaf proves the first page receives no partial write. |
| F — Translated atomic retry | `lwarx`/`stwcx.` and an authentic `bne` retry loop detect a second CPU's interfering `stw` through an effective alias, then complete the retried conditional store. |
| G — Context and mapping change | Two PIDs use separate root trees for the same EA; two configured plugin instances execute different code. PIDR/mapping/configuration changes are reflected without stale translations. |
| H — Exception return | `sc` enters a real-mode vector with IR/DR cleared; `rfid` restores relocation and execution continues at the translated address. |

Additional cases cover PRTS bounds/alignment, zero root descriptors, invalid RTS/NLS/entry forms, valid-but-unimplemented 64 KiB and larger leaves, 4 KiB leaf translation, R/C faults, supervisor-only pages, PIDR SPR access, v1/v2/v3 restore, and reservation invalidation across remapping and CPU/memory checkpoint restore.

Validation on the repository's MinGW build:

- Direct PPC64 executable results: Phase 1 **148/148**, Phase 2 **399/399**, Phase 3 **535/535**, Phase 4 **378/378**, Phase 5 **496/496**, and Phase 6 **183/183** checks passed.
- `ctest --test-dir build -R PPC64Phase[1-6]Tests --output-on-failure`: **6/6 passed**.
- The selected shared CTest set ran **6/11**: Basic, NewInstruction, EFI memory map, IA-64 SAL, VM checkpoint, and RISC-V decoder passed. Syscall, debugger, profiler, and fuzzer targets failed to link on unresolved WinSock (`__imp_*`) symbols; Ninja stopped before producing the VM isolation executable. These are the known MinGW `ws2_32` linkage failures, outside the radix changes. The runnable IA-64/shared tests passed. The legacy `test_isa_plugin` NaT serialization abort remains outside this run and this phase.

The shared CTest baseline is therefore partially unavailable in this toolchain. A manual `test_isa_plugin.exe` run reproduces the separate legacy NaT serialization assertion at `tests/test_isa_plugin.cpp:1551`. No IA-64 regression was observed in the shared suites that could be built and run.

## Unsupported and deferred behavior

This phase does not implement hash tables, a real PATE/LPID/partition radix stage, PAPR hypercalls, complete pSeries or Open Firmware startup, 64 KiB or other leaf sizes, I/O storage attributes, AMR/IAMR keys, TLB instructions/shootdowns, SMP scheduling, host-parallel execution, MMIO transactions, or Linux boot. The selected process tree cannot claim to model these configurations. Future pSeries boot work needs real PATE setup, PAPR process-table registration, partition-scoped address translation, guest firmware/platform interfaces, and broader page-size and device semantics.
