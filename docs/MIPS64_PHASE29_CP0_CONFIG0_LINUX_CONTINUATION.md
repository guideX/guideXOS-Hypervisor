# MIPS64-29 — CP0 Config0 and authentic OpenWrt continuation

**Outcome: A — complete for Config0 correctness.** The accepted OpenWrt Malta
BE64 kernel reached the authenticated Config0 read, retired it in the production
VM, and continued for eight more instructions to a different unsupported
instruction. Linux boot completion is not claimed. Config1 is exposed only as
the minimal report needed to back Config0.M; Config2 and later configuration
registers remain unsupported.

## 1. Git and worktree preflight

- Repository: `D:\dev\guideXOS_Hypervisor_MIPS64`; branch: `MIPS64`.
- Starting HEAD: `0653f0ee4e29418c2d90155907b50b40ca083933`, the expected
  MIPS64-29 base. The worktree was clean. `origin/MIPS64...HEAD` was `0 1`
  (zero behind, one ahead); no branch or Git topology changes were made.
- `.phase-mips64` recorded `last_completed=MIPS64-28` and
  `next_expected=MIPS64-29`. The IA-64 `.phase` marker and IA-64 source were
  left untouched.
- The Phase 28 report and accepted trace were inspected. The 375-record trace
  `docs/artifacts/openwrt_malta_be64_prid_phase28.jsonl` remains unchanged with
  SHA-256
  `3a14bb7df4f1f2f387373b40ba948d37b2c990980cafc28e2e3998a0ce68c76b`.
- Existing CP0 reads/writes, CPU construction and endian selection, KSEG0 CCA,
  TLB/MMU, LL/SC, CPU serialization and full-VM restore paths were inspected.
  The documented artifact directory `E:\CodexArtifacts\guideXOS_Hypervisor`
  was checked; the repository's existing authenticated kernel was used.

## 2. Kernel and trace identities

The unmodified OpenWrt 24.10.4 Malta BE64 / Linux 6.6.110 ELF is
`build/mips64-12-artifacts/openwrt-24.10.4-malta-be64-vmlinux.elf`, 11,013,776
bytes, SHA-256
`1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc`, entry
`0xffffffff80917670`. The accepted 256 MiB RAM and existing ROM/PROM handoff
were reused. No kernel, checkpoint, snapshot or historical trace was replaced.

The new bounded trace is
`docs/artifacts/openwrt_malta_be64_config0_phase29.jsonl`: 382 JSONL records,
SHA-256
`0b9248ad337e4aa5d0c434391b6c9ffb4d1ba3e8c28b26a0bdc0a5a339c2d9d2`.
It includes the pre-instruction register state, Config0 retirement, Linux's
following instructions, the actual stop, and deterministic replay from the
Config0 snapshot.

## 3. Authenticated frontier and prior failure

The accepted prefix reaches PC `0xffffffff8010b840`, physical fetch address
`0x00010b840`, with fetched word `0x40068000`. Its independent field decode is:

| Field | Value |
|---|---:|
| Opcode | COP0 (`0x10`) |
| `rs` | 0, MFC0 |
| `rt` | 6, `$a2` |
| `rd` | 16, CP0 Config |
| `sel` | 0, Config0 |
| Reserved bits 10:3 | 0 |
| Operation | `MFC0 $a2,$16,0` |

Phase 28 stopped because `readCp0(16,0,...)` had no supported register/selector
case, so the executor returned its internal `Cp0Violation`. The CPU did not
take a guest exception. As in the accepted Phase 28 trace, the failed probe
incremented Count without retirement; the valid replay state is restored to
the state before that probe, with Count, VM cycles and retired instructions
all at 158,205.

The authenticated prestate has `$a2=0x00000000007c0000`, PRId
`0x00018900`, Status `0x0000000000400080`, Cause/EPC/BadVAddr zero, EBase
`0x0000000080000000`, kernel privilege, and no armed delay slot. The full GPR
prestate and a valid full-VM v3 snapshot are in the trace.

## 4. Config0 value and field justification

The selected profile-derived values are:

| Field | Bits | BE64 | LE | Reason |
|---|---:|---:|---:|---|
| M | 31 | 1 | 1 | Config1 is present and readable |
| K23 | 30:28 | 0 | 0 | No separate KSEG2/3 cache attribute is modeled |
| KU | 27:25 | 0 | 0 | No separate user-segment cache attribute is modeled |
| Impl | 24:16 | 0 | 0 | No implementation-defined Config0 bits are claimed |
| BE | 15 | 1 | 0 | Matches the configured CPU byte order |
| AT | 14:13 | 2 | 2 | MIPS64 address-segment architecture |
| AR | 12:10 | 1 | 1 | MIPS64 Release 2 |
| MT | 9:7 | 1 | 1 | Standard TLB MMU, matching the existing JTLB |
| Reserved | 6:4 | 0 | 0 | Reserved bits read as zero |
| VI | 3 | 0 | 0 | No virtually indexed/tagged I-cache is modeled |
| K0 | 2:0 | 3 | 3 | Existing KSEG0 cached noncoherent CCA policy |

The independently assembled BE word is **`0x8000c483`**; LE is
**`0x80004483`**. The tests use those literal expected values and independently
check each field and all reserved/unsupported fields. Config0 is generated
from one authoritative CPU-profile definition and the configured endianness.

The fixed CPU profile uses PRId `0x00018900`, MIPS64 Release 2 operations, a
40-bit virtual-segment model, 36-bit physical addresses and 16 JTLB entries.
Linux's pinned `cpu.h` identifies company `0x01` and implementation `0x89` as
MIPS/5KE; the pinned CPU probe identifies that family as 5KE. This remains a
**5KE-compatible integer subset**, not a claim of full silicon equivalence.
The architectural fields agree with the implemented ISA and standard TLB.
The CPU has no FPU path and does not model physical cache arrays or complete
coherence. K0 reports the existing KSEG0 translation CCA; it does not claim
that data or instruction caches exist in the emulator.

The [MIPS64 PRA, Volume III, revision 2.50](https://0x04.net/~mwk/doc/mips/MD00091-2B-MIPS64PRA-AFP-02.50.pdf)
defines the Config0 fields and Config.M chaining. QEMU's [5KEc definition](https://gitlab.com/qemu-project/qemu/-/blob/master/target/mips/cpu-defs.c.inc)
confirms the AT/AR/MT identity for that PRId, but advertises a different cache,
watch/debug, and TLB profile and uses K0=2 in its common Config0 initializer.
Those QEMU values were not copied. guideXOS uses its own observed 16-entry
TLB and established CCA 3 policy.

## 5. Config.M chain and minimal Config1 scope

Config0.M is 1. It is not cleared to conceal the absence of Config1. Linux
6.6.110's `decode_config0()` uses Config0.M to continue the configuration
chain; `decode_config1()` reads MMUSize as `((Config1 >> 25) & 0x3f) + 1` and
returns Config1.M to decide whether Config2 follows. The [pinned Linux source](https://raw.githubusercontent.com/gregkh/linux/v6.6.110/arch/mips/kernel/cpu-probe.c)
maps AT=2/AR=1 to MIPS64r2 and MT=1 to a standard TLB.

The minimal Config1 value is **`0x1e000000`**: MMUSize is 15, encoding the
actual 16 JTLB entries; the cache line-size and optional feature fields remain
zero; Config1.M is zero, so Config2 is not advertised. It is exposed at CP0
16/1 so Config0.M is backed by a coherent register chain. This is intentionally
limited to Config1; Config2 and later selectors still raise the existing
unsupported-CP0 fault. Config1 does not advertise an FPU, performance counter,
watch, EJTAG, MIPS16, coprocessor-2, or physical I/D cache arrays. Config0.K0
still reports the existing KSEG0 CCA 3 policy.

The authentic continuation does **not** read Config1 before stopping. The
guest's recorded path first checks Config0.MT; it executes no Config1 MFC0 in
the eight post-Config0 retirements. Config1 behavior is instead qualified by
encoded CPU/VM tests. The source audit describes how Linux would decode the
fields; it is not evidence that this guest run completed the whole probe.

## 6. K0, writes and CPU feature boundaries

KSEG0's existing address translation and memory policy use CCA 3 (cached,
noncoherent). Config0.K0 returns 3 for both byte orders. The translation and
LL/SC paths remain unchanged. Although architectural Config0.K0 is writable,
the emulator has no mutable cache policy to apply consistently: the Config
register is not accepted by `writeCp0()`, so MTC0/DMTC0 writes to Config0 or
Config1 remain unsupported and cannot silently claim a new K0. The profile
fields are fixed reports, not serialized mutable state.

Only Config0/Config1 reads were added. PRId and EBase behavior is unchanged;
unknown selectors and Config2 remain unsupported. No new CPU state or snapshot
version was introduced.

## 7. Encoded CPU and VM qualification

`test_mips64_config0` reported **528 passed, 0 failed**. Expected values are
independent literals. The suite checks Config field layout and endian values;
MFC0 sign extension and deterministic DMFC0 zero extension; destination
`$zero`; source/destination isolation; repeated reads; reset; both endian
configurations; CPU0/CPU1 and independent-VM consistency; PRId and EBase
separation; unknown selectors; Config0/Config1 write rejection; Config1's
16-entry TLB size and M=0; kernel/user privilege; and unchanged RAM, TLB,
HI/LO, reservation and non-Count CP0 state.

CPU serialization replay, taken and not-taken branch delay slots, delay-slot
serialization, LL reservation with CCA 3, full-VM snapshot replay and
transactional malformed-snapshot rejection are covered. The authentic
production VM test captures immediately before Config0, retires it, restores,
repeats, and continues to the identical stop. PRId-focused tests remain
**424/424**.

Snapshot versions and layouts remain unchanged: full VM `GXM64SNP` v3; CPU v6
(1,001 bytes); memory v2; UART v4; IRQ routing v1. Restore itself does not run
guest instructions or advance virtual time.

## 8. Authentic Config0 retirement

The production Malta VM fetched and retired the exact authenticated word at
`0xffffffff8010b840`. MFC0 uses the existing 32-bit CP0 transfer semantics,
sign-extending bit 31:

| Observation | Before | After |
|---|---:|---:|
| `$a2` | `0x00000000007c0000` | `0xffffffff8000c483` |
| PC | `0xffffffff8010b840` | `0xffffffff8010b844` |
| Count | 158,205 | 158,206 |
| VM cycles | 158,205 | 158,206 |
| Retired instructions | 158,205 | 158,206 |

No guest exception occurred. Status, Cause, EPC, BadVAddr, EBase, PRId,
other GPRs, HI/LO, TLB, RAM, reservation and UART/console state were
unchanged apart from normal PC/Count/retirement progression. The previous
failed Config0 probe's Count increment was not mixed into this successful
state.

## 9. Linux consumption and next authentic frontier

The trace records eight instructions after Config0. Linux first forms the
expected TLB mask, executes `andi $v0,$a2,0x0380`, compares the result with
`0x80`, and takes the branch with its normal delay slot. This is consistent
with Config0.MT=1. The inspected Linux decoder maps AT=2/AR=1 to MIPS64r2 and
MT=1 to standard TLB; the run does not independently demonstrate that Linux
completed those later decode paths.

The next stop is an unsupported instruction at PC
`0xffffffff8010b85c`, fetch PA `0x00010b85c`, word `0x7cc30b40`. Its
independent SPECIAL3 decode is `EXT $v1,$a2,1,14` (rs=6, rt=3, rd=1, sa=13,
funct=0), outside the implemented DEXT subset. It is not a CP0 Config1 read,
and no architectural exception was entered. The stop is honest and does not
reflect failure of Config0.

At the snapshotable stop: 158,214 total instructions were retired, 158,197
since Linux entry, and eight after the Config0 retirement. Count and VM cycles
are 158,214; Status is `0x400080`; Cause/EPC/BadVAddr are zero; no delay slot
or reservation is armed/valid. UART TX/RX and console byte/sequence counts are
zero. MMIO was **not instrumented**, so no MMIO count or device conclusion is
claimed. The bounded continuation budget was 1,000,000 instructions; execution
stopped earlier at the unsupported EXT.

## 10. Snapshot replay evidence

The recorded pre-Config0 full-VM v3 snapshot was restored, MFC0 was repeated,
and the same Config0 result and post-state were obtained. The restored run then
retired the same eight instructions and reached the same EXT word, PC, stop
reason, Count and VM cycles. CPU bytes, RAM, reservation generations, device
state, UART/console, scheduler state and counters matched. A malformed snapshot
was rejected transactionally. Synthetic BE and LE snapshots reproduce their
own Config0 values; no endianness conversion was added to serialized CPU
state.

## 11. Regression and production linkage

- Build: 402-step rebuild succeeded for `ia64emu`, `ia64emu_tests`, the
  affected MIPS64 test targets and all MIPS64 test executables. The changed
  legacy CP0 exception test was rebuilt afterward as well.
- MIPS64 CTest: **57/57 passed** (56 inherited suites plus Config0).
- Selected cross-ISA regressions: **6/6 passed** — BasicTests,
  NewInstructionTests, EfiMemoryMapTests, IA64SalTests, VMCheckpointTests,
  and RiscVDecoderSmokeTest.
- Production linkage: `objdump -t build/bin/ia64emu.exe` contains the linked
  `Mips64Cpu::readCp0(unsigned,unsigned,unsigned long&)` symbol
  (`_ZNK6mips649Mips64Cpu7readCp0EjjRy`) in the executable's text section.
- The old CP0 exception suite had a stale assertion that PRId was unsupported.
  Its unsupported-pair list now tests Config2 and other unsupported selectors;
  the suite reports 480 passed, 0 failed. No production exception behavior was
  changed to accommodate this test update.

## 12. Limits and MIPS64-30 recommendation

This milestone establishes Config0 correctness and the minimum Config1 report
needed for Config0.M. It does not establish complete 5KEc emulation, physical
cache behavior, a full cache-coherence model, writable K0 behavior, FPU or
optional coprocessor features, Config2–Config7, or Linux boot completion.
Config1 was not accessed by the authentic run before its EXT stop.

Recommended MIPS64-30 scope: implement and qualify only the authenticated
`EXT` instruction encoding and semantics encountered here, including operand
field boundaries, MIPS64r2 behavior, endian-independent instruction fetch,
delay-slot behavior, CPU/full-VM replay and unchanged state on faults. Then
continue the same preserved kernel and stop again at the next observed
frontier; do not add speculative Config2+ or Malta features.
