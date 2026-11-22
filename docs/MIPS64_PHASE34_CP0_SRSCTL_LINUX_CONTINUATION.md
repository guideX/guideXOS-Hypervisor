# MIPS64-34 — CP0 SRSCtl and authentic OpenWrt continuation

**Outcome A — complete for the guideXOS MIPS64 Release 2 subset.** The
production Malta VM retired Linux's authenticated `MFC0 $v0,$12,2`, reported
the single ordinary GPR set truthfully, replayed the full VM snapshot
deterministically, and continued to the next unsupported instruction. Linux
boot completion is not claimed.

## 1. Repository preflight and preserved inputs

- Repository: `D:\dev\guideXOS_Hypervisor_MIPS64`.
- Branch: `MIPS64`; starting full HEAD:
  `c6259f830d5084bbc06a077b1241a7c21ceff0f3`.
- Starting worktree was clean. `origin/MIPS64...HEAD` was `0 0`.
- `.phase-mips64` recorded MIPS64-33 complete and MIPS64-34 next. No
  `AGENTS.md` was present in the repository or its parent locations.
- The MIPS64-28 PRId, MIPS64-29 Config0 and MIPS64-33 MOVN reports were read.
  The MIPS64-specific artifact directory and the relocated artifact root were
  inspected. The existing repository kernel and ROM/PROM handoff were reused;
  no guest image, historical trace, checkpoint or snapshot was regenerated.
- No branch switch, worktree creation, reset, rebase, merge, cherry-pick,
  stash, clean, topology change or push was performed. IA-64 source and its
  separate `.phase` marker were left untouched.
- All **61 inherited MIPS64 CTest cases passed before editing**.

## 2. Authenticated kernel and Phase 33 baseline

| Property | Value |
|---|---|
| Distribution / platform | OpenWrt 24.10.4 / Malta BE64 |
| Linux | 6.6.110 |
| ELF | `build/mips64-12-artifacts/openwrt-24.10.4-malta-be64-vmlinux.elf` |
| ELF size / SHA-256 | 11,013,776 bytes / `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc` |
| Entry | `0xffffffff80917670` |
| RAM / endianness | 256 MiB / big-endian |
| ROM/PROM | Existing Malta handoff; ROM base `0x1fc00000`, size 4 MiB |
| ROM content identity | `0xfa887be906ff0340` |

The accepted Phase 33 trace remains four records with SHA-256
`d4d37456d4836aafcd31e4efab7d7d64bda05f6be46e132e0d5a7cdd7e0f1f28`.
It records 158,473 successful retirements before the MFC0 attempt at
`0xffffffff8010c1e4`. The pre-attempt snapshot is the valid checkpoint; the
following Count increment belongs to a failed attempt, not retired execution.
The Phase 33 four-record trace was checked by hash and by the overlapping
frontier observations. No artificial 256-record match was required.

## 3. Frontier reproduction and instruction decode

The production Malta VM replayed the accepted startup prefix and unchanged
kernel to virtual PC `0xffffffff8010c1e4`, physical fetch address
`0x00010c1e4`. The fetched bytes were `40 02 60 02`, independently assembled
as word `0x40026002`:

| Field | Value |
|---|---:|
| Primary opcode | COP0, `0x10` |
| `rs` | 0 — MFC0 |
| `rt` | 2 — `$v0` |
| `rd` | 12 — CP0 register 12 |
| `sel` | 2 — SRSCtl |
| Reserved bits 10:3 | 0 |
| Operation | `MFC0 $v0,$12,2` |

Before this change `requireKernel()` succeeded, but `readCp0(12,2,...)` had no
matching selector case. The MFC0 executor therefore returned its internal
`Cp0Violation` (“MFC0 to unsupported CP0 register/selector”). It did not raise
a guest exception. The failed attempt left PC at the instruction, advanced
Count from 158,473 to 158,474, and did not advance VM cycles or retired
instructions. This was reproduced before editing and matched the accepted
Phase 33 record.

## 4. SRSCtl value and CPU-profile consistency

MIPS64 PRA Volume III revision 2.50, §8.20, defines SRSCtl at CP0 12/select 2.
HSS occupies bits 29:26 and is the **highest implemented shadow-set number**:
HSS=0 means only the normal GPRs; a value `n` means sets 0 through `n`.
EICSS is bits 21:18, ESS bits 15:12, PSS bits 9:6, and CSS bits 3:0. Reserved
fields read zero. See the [MIPS64 Privileged Resource Architecture, §8.20](https://0x04.net/~mwk/doc/mips/MD00091-2B-MIPS64PRA-AFP-02.50.pdf).

The implementation has one `std::array` GPR bank. Reset clears that array;
exception entry updates Cause/EPC/Status and redirects PC without selecting a
different bank; ERET restores EPC or ErrorEPC and clears EXL/ERL without a GPR
bank selection. There is no complete EIC/vector-shadow implementation.
Consequently SRSCtl.HSS=0 is the truthful value; it does not mean “one shadow
bank.” The immutable profile value is `0x00000000`:

| Field | Bits | Observed value |
|---|---:|---:|
| HSS | 29:26 | 0 |
| EICSS | 21:18 | 0 |
| ESS | 15:12 | 0 |
| PSS | 9:6 | 0 |
| CSS | 3:0 | 0 |
| Reserved | remaining | 0 |

The surrounding profile remains consistent: PRId is `0x00018900` (documented
as a 5KE-compatible integer subset); Config0 reports MIPS64r2 and a standard
TLB; Config1 reports 16 entries and no FPU. Config0/Config1 do not claim
shadow-set or vectored-EIC capabilities, and Config1 does not advertise a
Config2/Config3 chain. The report is derived from the no-shadow CPU profile,
not from a guest PC special case. No second GPR bank or selector fallback was
added.

## 5. Linux interpretation and observed operands

The pinned [Linux 6.6.110 CPU probe](https://raw.githubusercontent.com/gregkh/linux/v6.6.110/arch/mips/kernel/cpu-probe.c)
uses `srsets = ((read_c0_srsctl() >> 26) & 0x0f) + 1` when probing MIPS Release
2/6. The [pinned CPU definitions](https://raw.githubusercontent.com/gregkh/linux/v6.6.110/arch/mips/include/asm/cpu.h)
identify the processor family from PRId. At the authentic frontier, `$v0`
before the read was `0x000000000001228b`; the guest-visible SRSCtl result was
`0x0000000000000000`, so MFC0's 32-bit sign extension produced zero.

The unchanged guest then retired `EXT $v0,$v0,26,4`, producing HSS=0;
`ADDIU $v0,$zero,1` produced the set count; and `SW $v0,132($a0)` stored
`0x00000001` at guest address `0xffffffff80a8a984` (physical
`0x000a8a984`). Thus Linux's `srsets=1` was observed in executed guest state,
not inferred solely from the C expression. The subsequent 47 retirements did
not access IntCtl, SRSMap, or another shadow-set operation before the next
stop.

## 6. CP0 selector and write policy

SRSCtl is an explicit CP0 12/select 2 case. MFC0 returns the 32-bit register
value with the existing sign-extension path; DMFC0 returns zero extended to
the 64-bit GPR. Status remains CP0 12/select 0. IntCtl 12/1 and SRSMap 12/3
remain unsupported and still produce `Cp0Violation`; no general zero-return
fallback was added.

The architecture marks HSS read-only and CSS/PSS/ESS read/write. With HSS=0,
zero is the only defined value for those fields; EICSS and reserved fields
must also be zero when written. This subset therefore accepts MTC0/DMTC0
writes of zero as no-ops and deterministically rejects nonzero writes with
`Cp0Violation`, rather than inventing shadow state for architecturally
undefined requests. Reads remain zero after writes and reset. SRSCtl adds no
mutable CPU state and does not change serialization versions.

## 7. Qualification and snapshots

`Mips64SrsctlTests` covers both instruction endiannesses, MFC0 and DMFC0,
selector isolation, legal-zero and rejected-nonzero MTC0/DMTC0, reset
stability, user-mode CP0 privilege rejection, exception entry and ERET with
the single GPR bank, and an armed branch-delay-slot SRSCtl read. The full-VM
delay-slot snapshot restores and reproduces the same destination, branch
completion, counters and snapshot state. A checksum-invalid snapshot is
rejected transactionally without changing CPU state, VM cycles or retirement.

Accepted snapshot formats remain full VM `GXM64SNP` v3, CPU v6 (1,001 bytes),
memory v2, UART v4, and IRQ routing v1. Synthetic and authentic qualification
require no snapshot version or layout change.

## 8. Authentic retirement and bounded continuation

The production VM captured a valid full-VM v3 snapshot immediately before
the authentic instruction (checksum `0xb617e6fb9a047742`). The prestate had
Status `0x00400080`, Cause/EPC/BadVAddr zero, EBase `0x80000000`, kernel
privilege, no armed delay slot and no active reservation. MFC0 retired
normally:

| Observation | Before | After MFC0 SRSCtl |
|---|---:|---:|
| `$v0` | `0x000000000001228b` | `0x0000000000000000` |
| PC | `0xffffffff8010c1e4` | `0xffffffff8010c1e8` |
| Count / VM cycles / retired | `158473 / 158473 / 158473` | `158474 / 158474 / 158474` |
| Delay slot | clear | clear |
| Guest exception | none | none |

The full VM was restored to the pre-instruction snapshot, the same MFC0 and
continuation were repeated, and CPU state, CP0, RAM, reservation generations,
TLB, device/UART/console, scheduler, counters and final stop snapshot matched.
The bounded allowance was one million attempted instructions; execution
stopped earlier at a valid-but-unsupported instruction:

| Observation | Value |
|---|---:|
| Total retired | 158,521 |
| Retired since Linux entry | 158,504 |
| Retirements beyond former SRSCtl frontier | 48 (MFC0 plus 47) |
| Next PC / fetch PA | `0xffffffff8010c384` / `0x00010c384` |
| Raw bytes / word | `70 42 10 24` / `0x70421024` |
| Decode status | valid MIPS64 opcode outside implemented subset |
| Stop Count / VM cycles / retired | `158521 / 158521 / 158521` |
| Architectural exception / delay slot | none / clear |

At the stop, Status remained `0x00400080`; Cause, EPC and BadVAddr remained
zero. UART TX, UART RX and console output were zero bytes. MMIO statistics
were not instrumented and are not claimed. The guest did not boot.

## 9. Trace, build and regression status

The bounded 51-record trace is
[`docs/artifacts/openwrt_malta_be64_srsctl_phase34.jsonl`](artifacts/openwrt_malta_be64_srsctl_phase34.jsonl),
SHA-256 `832429de72b9f33dd3ad30be34a9a9ea843c8894e6c6ad83c9fb7404d5c69ee`.
Every line parses as JSON. It records the authenticated prestate, raw decode,
actual previous/result `$v0`, SRSCtl fields, Linux's stored `srsets`, the
retirement/counter progression, continuation instruction window, next stop,
device observations and deterministic replay. Historical traces remain
unchanged.

- Focused SRSCtl test total: **198 passed, 0 failed**.
- Inherited MIPS64 baseline before editing: **61/61 passed**.
- Final MIPS64 CTest: **62/62 passed**.
- Selected cross-ISA regressions (`BasicTests`, `NewInstructionTests`,
  `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`, and
  `RiscVDecoderSmokeTest`): **6/6 passed**.
- Production `ia64emu.exe`: target build succeeded; `nm -C` confirms linked
  `Mips64Cpu::readCp0` and `Mips64Cpu::writeCp0` symbols.
- Full aggregate `ninja -C build all -j6`: **failed only while linking the
  unrelated `debug_harness.exe` target**, with unresolved MinGW Winsock imports
  including `__imp_accept`, `__imp_send`, `__imp_closesocket`,
  `__imp_WSACleanup`, and `__imp_WSAGetLastError`. The focused SRSCtl and
  production targets built successfully; no networking or debug-harness
  source was changed.

## 10. Limits and MIPS64-35 recommendation

This phase implements one CP0 selector for the existing CPU subset. It does
not implement extra GPR banks, SRSMap, IntCtl, vectored EIC, shadow-set
instructions, additional Config registers, or full 5KEc hardware. Linux boot
completion is unverified. The new next frontier is word `0x70421024` at
`0xffffffff8010c384`; MIPS64-35 should independently decode and qualify that
authenticated instruction, then continue the same kernel to its next genuine
frontier. Do not add speculative shadow-register features.
