# MIPS64 Phase 15: LLD, SCD, and atomic reservation state

## Outcome

**Outcome A — Complete.** The authenticated Linux stream requires both LLD and SCD in the
same update path, so this phase implements and qualifies that pair. The kernel
retires both operations and reaches the next unsupported instruction without
an architectural exception. Linux boot is not claimed.

## Preflight and kernel identity

The task began in `D:\dev\guideXOS_Hypervisor_MIPS64` on branch `MIPS64` at
`15d89949ae0b586b5f5a57555bcf253553099834`, with no local changes, upstream
aligned, and `.phase-mips64` reporting MIPS64-14 complete and MIPS64-15 next.
The Phase 12–14 reports, accepted traces, and all 40 accepted MIPS64 CTest
suites were reviewed before implementation. No branch, worktree, or Git
topology changes were made.

The unchanged accepted kernel is OpenWrt 24.10.4 Malta BE64:

| Property | Value |
|---|---|
| ELF | `build/mips64-12-artifacts/openwrt-24.10.4-malta-be64-vmlinux.elf` |
| Size | 11,013,776 bytes |
| SHA-256 | `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc` |
| Entry | `0xffffffff80917670` |
| Guest RAM | 256 MiB |
| Handoff | Accepted Malta ROM/PROM configuration |

No kernel, ROM/PROM image, or firmware was patched or regenerated.

## Authenticated LLD frontier

The production MIPS64 VM reproduced the Phase 14 prestate exactly. At 150,313
retired instructions from reset (150,296 from Linux entry), it fetched the
word `0xd0850000` at VA `0xffffffff8091a338`, PA `0x00091a338`. Independent
field decoding gives opcode `0x34`, `rs=4`, `rt=5`, signed offset `0`:
`LLD $5,0($4)`. Immediately before execution, `$4=0xffffffff80b0d078`,
`$5=1`, Status=`0x00400080`, and Cause/EPC/BadVAddr are zero. The data
translation resolves to PA `0x000b0d078`; the eight RAM bytes are
`0000000000000000`, so BE64 expects the loaded value `0x0000000000000000`.
The address is 8-byte aligned and the access is dense guest RAM.

The trace includes the decoded fields, preinstruction registers and CP0
state, physical translation, CCA, bytes, loaded value, and the post-LLD
reservation identity. The first 256 instruction records match the Phase 14
trace byte-for-byte (zero mismatches).

## LLD/SCD stream evidence and scope

The authentic instructions around the frontier are:

```text
ffffffff8091a338  d0850000  lld   $5,0($4)
ffffffff8091a33c  14a00005  bnez  $5,ffffffff8091a354
ffffffff8091a340  00000000  nop
ffffffff8091a344  00600825  move  $1,$3
ffffffff8091a348  f0810000  scd   $1,0($4)
ffffffff8091a34c  1020fffa  beqz  $1,ffffffff8091a338
ffffffff8091a350  00000000  nop
```

The loaded zero takes the update path. The authentic SCD at
`0xffffffff8091a348` sees the active LLD reservation and stores the original
`$1` value `0xffffffff80a8fb40` to PA `0x000b0d078`; it then writes `1` back
to `$1` and consumes the reservation. The following retry branch is not
taken. This is direct recorded evidence of a successful guest SCD, so SCD was
implemented rather than left unsupported or made to report unconditional
success.

The instructions execute normally. The authenticated SCD is reached four
retired instructions after LLD; the VM then retires another 36 instructions
after LLD before reaching the next unsupported word.

## Reservation and coherency model

MIPS64 Release 2 defines LLD/SCD behavior for cached memory access types; it
does not define these operations for uncached targets. The current fixed Malta
CPU profile models KSEG0 as cached noncoherent CCA 3 and KSEG1 as uncached CCA
2. Cached CCA 3 and 5 are supported for LLD/SCD; an LLD on an unsupported
uncached CCA is deterministically rejected as a data-bus error, while an SCD
with an incompatible target/CCA fails with result zero and no store. This
policy makes the support boundary explicit. Caches and the complete hardware
coherence protocol are not simulated.

The CPU stores a reservation containing valid state, access size, CCA, full
virtual address, translated physical address, dense-RAM backing ID, aligned
reservation-granule offset, and memory generation. Each CPU owns its CPU-local
reservation. Each VM's `PhysicalAddressMap` owns its own monitor and mutex;
there is no process-global reservation state.

The monitor uses a 16-byte granule aligned relative to the selected RAM
backing. MIPS64 leaves the reservation block implementation-specific; 16 bytes
is the emulator's documented policy, not a claim that the architecture fixes
that size. Generation identity is backing-relative, so aliases to the same
physical backing share conflict tracking. SCD also requires the same virtual
address, physical address, size, and CCA as the active LLD.

LLD reads and records the current generation under the monitor lock. SCD
checks the token and commits the 8-byte store under that same lock, then
advances the generation. Ordinary writes invalidate only tracked granules
that overlap their byte range. Guest bus stores, `Memory::Write`, host bulk
loads, RAM batch transactions, clear, and memory snapshot restore route
through generation-tracked RAM APIs. RAM writes outside these APIs are outside
the supported coherency contract. ROM, sparse RAM, and MMIO are not SCD
targets; a successful conditional store is restricted to dense writable RAM.

LLD replaces a CPU's previous reservation. SCD consumes it on success,
failure, and fault. ERET, CPU reset, and endianness reconfiguration clear the
reservation. Reads and unrelated writes do not invalidate it. Conflicting
writes to the same physical 16-byte granule advance its generation; writes
outside that granule do not. The VM monitor serializes competing SCDs and
tracked writes. This is a deterministic software reservation model, not a
cache-coherence simulator or a claim about host-parallel SMP.

## Encoded instruction and concurrency coverage

The new `Mips64LldScdTests` suite exercises encoded guest words. Coverage
includes the authenticated LLD word, decode/disassembly, zero/positive/
negative offsets, BE and LE loads/stores, high-bit values, base/target aliasing,
`$zero`, alignment and translation faults, TLB and address modes, reservation
replacement, fault atomicity, ERET/reset invalidation, ROM/MMIO/unmapped
targets, host-write invalidation, memory snapshots, and production VM
execution.

Two CPU tests prove that both CPUs can reserve the same unchanged granule,
the first SCD wins, and the second fails. A same-granule CPU1 write forces
CPU0's encoded SCD to fail; an encoded retry loop then performs a fresh LLD
and successfully updates the value. A write 16 bytes away does not force an
invalidation. Two VMs with the same guest physical address retain isolated
monitors.

## Snapshot and replay compatibility

Active reservation state is execution state, so the formats were versioned:

| Component | Previous | Current |
|---|---:|---:|
| Full VM snapshot | v2 | v3 |
| CPU serialization | v5, 957 bytes | v6, 1,001 bytes |
| Physical memory component | v1 | v2 |
| UART | v4 | v4 |
| IRQ routing | v1 | v1 |

CPU v5 and full-VM v2 snapshots remain supported and migrate without an
active reservation. The v3 memory snapshot records reservation tracking
version 1, backing/granule generations, and the next generation counter.

The production runner captures a full VM snapshot at the exact pre-LLD PC,
executes the authentic LLD, restores the pre-instruction snapshot, and
executes LLD again. CPU serialization, loaded result, reservation identity
and generation, target bytes, instruction count, and cycles match across both
executions. It then captures a second snapshot with the active reservation,
restores it, and replays through the same SCD and next frontier. CPU/memory
state, guest output, instruction counts, cycles, UART state, and reservation
generations match. Restore advances the console's restore-epoch generation
from 3 to 4, so replay comparison checks console bytes rather than requiring
the epoch number to repeat.

A VM test also snapshots a stale CPU token after a conflicting write changed
the memory generation. Replaying SCD after restore returns failure, leaves
the target doubleword unchanged, and reproduces the same memory and monitor
state. Rejected/corrupted snapshots are checked for integrity and transactional
failure.

## Authentic Linux continuation and stop

The unchanged kernel retired 150,350 instructions from reset and 150,333 from
Linux entry. That is 309 retired instructions beyond the former DSRL32
frontier and 63 beyond the SLTIU frontier. LLD and SCD both retired normally;
36 instructions retired after LLD. The next frontier is:

| Observation | Value |
|---|---|
| VA / physical fetch | `0xffffffff801caf50` / `0x0001caf50` |
| Raw instruction | `0x00040336` |
| Decode | `UnsupportedInstruction` |
| Stop | Clean predecode boundary; unsupported instruction not executed |
| CP0 Count / VM cycles | 150,350 / 150,350 |
| Architectural exception | None |
| Valid TLB entries | 0 |
| UART bytes | 0 |
| VM state at stop | STOPPED |

The bounded Phase 15 trace is
[`openwrt_malta_be64_lld_phase15.jsonl`](artifacts/openwrt_malta_be64_lld_phase15.jsonl).
It contains 575 JSONL records, is 172,645 bytes, and has SHA-256
`310be283940068f5570bfc02bc0581dd127c8b505692809c7f0628ff544868cc`.
Earlier Phase 12–14 traces remain unchanged.

## Regression and production linkage

All 42 MIPS64 CTest suites passed with 10,941 assertions. The focused encoded
LLD/SCD suite contributed 129 assertions, the reservation snapshot/replay
suite 41, and the CPU serialization migration suite 121. The five selected
IA-64 regression suites and the RISC-V decoder smoke suite also passed (6/6).

The production `ia64emu.exe` target built successfully. `nm -C` confirmed the
linked executable contains `Mips64Cpu::executeMemory` and the monitor's
`PhysicalAddressMap::ReadLinked` and `PhysicalAddressMap::StoreConditional`
symbols. The broader `all` target also attempted to build the unrelated
`debug_harness`, which fails to link Winsock imports (`accept`, `send`, and
related symbols); this does not affect the explicit production or MIPS64 test
targets. The known IA-64 NaT assertion in `tests/test_isa_plugin.cpp:1551`
and six unavailable legacy CTest executables remain separate historical
limitations; no IA-64 source or phase marker was changed.

## Limitations and recommended MIPS64-16 scope

The emulator does not complete an OpenWrt boot or produce UART output. It
does not simulate cache contents, full coherent/noncoherent hardware behavior,
general host-parallel SMP, or LL/SC word instructions. The current Malta
KSEG0 CCA is a fixed modeled profile rather than a guest-programmable CP0
Config.K0 register. Reservation operations support dense RAM through tracked
physical-map APIs; direct external writes outside that API are unsupported.

Recommended MIPS64-16 scope: authenticate and independently decode
`0x00040336` at `0xffffffff801caf50`, inspect the containing Linux routine and
surrounding bytes, implement only the instruction or behavior demanded by
that stream, and continue the same kernel to the next genuine frontier.

## References

- [MIPS64 Architecture for Programmers, Volume I, Revision 2.50](https://0x04.net/~mwk/doc/mips/MD00083-2B-MIPS64INT-AFP-02.50.pdf) — memory access types and CCA rules.
- [MIPS64 Architecture for Programmers, Volume II, Revision 2.50](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf) — LLD, SCD, and ERET operation.
