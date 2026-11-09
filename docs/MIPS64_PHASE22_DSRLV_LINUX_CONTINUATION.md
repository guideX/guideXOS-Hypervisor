# MIPS64-22 — DSRLV Architectural Qualification and OpenWrt Continuation

**Outcome A — complete.** The authentic OpenWrt DSRLV frontier was reproduced, independently decoded, implemented as a full-width logical right shift, retired by the production Malta VM, and replayed deterministically from full-VM snapshots. The unchanged Linux stream retired 500 instructions after DSRLV before reaching the next valid but unsupported instruction. Linux boot completion is not claimed.

## 1. Repository preflight

- Checkout: `D:\dev\guideXOS_Hypervisor_MIPS64`; branch: `MIPS64`.
- Starting HEAD: `30993573ee36bcff20eac1984f5e41427290d1c5`, as required.
- Initial worktree: clean. Ignored build output and the existing MIPS64 ELF fixture objects were preserved.
- At preflight, `origin/MIPS64` was recorded at `5ba7c4f`, leaving local `MIPS64` two commits ahead and zero behind. During qualification, the remote-tracking ref advanced to the starting HEAD `3099357`; its reflog records an `update by push` at `2026-11-08 20:10 -0800`. No push or Git topology operation was performed for MIPS64-22. At the final local commit, the branch will be one commit ahead of the then-current `origin/MIPS64`.
- `.phase-mips64` recorded MIPS64-21 complete and MIPS64-22 next. The IA-64 `.phase` marker was not modified.
- MIPS64-20 and MIPS64-21 reports, Phase13 DSRL32 and Phase17 SLLV reports, the DSLLV qualification, CPU decoder/executor/disassembly, and snapshot interfaces were inspected.
- All 49 inherited MIPS64 CTest suites passed before implementation.
- `E:\CodexArtifacts\guideXOS_Hypervisor` and the repository-documented MIPS64 artifact location were inspected. The existing authenticated ELF under ignored `build/mips64-12-artifacts` was reused; no accepted image or prior trace was overwritten.

## 2. Authenticated kernel and Malta configuration

| Property | Value |
|---|---|
| Distribution / target | OpenWrt 24.10.4 / Malta BE64 |
| Linux version | 6.6.110 |
| ELF | `build/mips64-12-artifacts/openwrt-24.10.4-malta-be64-vmlinux.elf` |
| Size | 11,013,776 bytes |
| SHA-256 | `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc` |
| ELF entry | `0xFFFFFFFF80917670` |
| Guest RAM / endian | 256 MiB / big-endian |
| PROM command line | `console=ttyS0,38400n8` |
| ROM | 4 MiB, existing Malta reset/PROM handoff |

The kernel was not regenerated, patched, skipped, or modified. The ELF hash was rechecked after the authentic run.

## 3. Phase21 baseline and frontier reproduction

The accepted Phase21 trace is `docs/artifacts/openwrt_malta_be64_dsllv_phase21.jsonl`, with 264 records and SHA-256 `5dd2aed393017e0f1b2d41aa7b1f834cd29643f9f45fd1a5fa142c1719681602`. A fresh pre-implementation production replay reproduced all 264 records and the exact hash. The trace’s 256 initial instruction records also match the first 256 records of the Phase22 trace byte-for-byte.

The unchanged kernel retired 151,598 guest instructions and stopped before the instruction at VA `0xFFFFFFFF8012F5E0` (fetch PA `0x0012F5E0`). The exact raw word fetched from guest memory was `0x00C31016`.

## 4. DSRLV encoding and architectural semantics

The [MIPS64 Architecture for Programmers, Volume II, Revision 2.50, DSRLV entry](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf) defines DSRLV as a 64-bit logical right shift with the low six bits of `GPR[rs]` as the count. The independent [QEMU MIPS opcode definitions](https://qemu.googlesource.com/qemu/+/c52e67924fbdadfa00668248f5c075542943c54c/target-mips/translate.c) identify the adjacent DROTRV encoding separately.

| Field | Value |
|---|---|
| Primary opcode | `SPECIAL` (`0x00`) |
| `rs` | 6 (`$a2`), shift count |
| `rt` | 3 (`$v1`), 64-bit source |
| `rd` | 2 (`$v0`), destination |
| `sa` | 0 |
| Function | `0x16` |
| Assembly | `DSRLV $v0,$v1,$a2` |

Execution reads both source operands before writing `rd`, masks `GPR[rs]` with `0x3F`, shifts an unsigned `uint64_t` right, and writes through the normal GPR helper. This fills high bits with zero, preserves all 64 source bits before shifting, does not use signed-right-shift behavior, and does not add an exception or memory access.

The decoder accepts `sa=0` as DSRLV. The legal `sa=1` encoding remains distinct as valid-but-unsupported DROTRV. Other noncanonical `sa` values are reserved and retain reserved-instruction behavior. DSRLV is not treated as DSRAV, DSRL, or a rotate.

## 5. Actual Linux prestate and retirement

The VM reached the frontier through the existing production decoder and reset ROM, with no host-written register result. The observed prestate was:

| Observation | Value |
|---|---|
| Retired / attempted boundary | 151,598 / 151,598 |
| PC VA / fetch PA | `0xFFFFFFFF8012F5E0` / `0x0012F5E0` |
| Word / fields | `0x00C31016`; opcode `0`, `rs=6`, `rt=3`, `rd=2`, `sa=0`, `funct=0x16` |
| `$a2` | `0x0000000000000000` |
| `$v1` | `0x0000000000000000` |
| Previous `$v0` | `0xFFFFFFFF80A8AB80` |
| Effective shift, `$a2 & 0x3F` | 0 |
| Independent expected result | `0x0000000000000000 >> 0 = 0x0000000000000000` |
| Delay state / pending branch | clear / none |
| Status / Cause / EPC / BadVAddr | `0x00400080 / 0 / 0 / 0` |
| Count / VM cycles | 151,598 / 151,598 |
| HI / LO / LL reservation / valid TLB entries | `0 / 0 / inactive / 0` |

The result was calculated independently from the captured source and count. DSRLV retired normally: `$v0` became zero, PC advanced to `0xFFFFFFFF8012F5E4`, delay state remained clear, and Count, VM cycles, and retired instructions each advanced to 151,599. Status, Cause, EPC, and BadVAddr did not change. No other GPR changed.

The authentic DSRLV is not in a branch delay slot. Separate encoded tests exercise the normal existing branch-delay path for both taken and not-taken branches, including snapshot/restore of an armed delay slot; DSRLV contains no special control-flow path.

## 6. Implementation and focused qualification

The implementation appends `Mips64Op::Dsrlv` to preserve previous enum values; recognizes SPECIAL function `0x16`; separates DSRLV, DROTRV, and reserved `sa` values; performs the masked unsigned 64-bit shift; and adds `dsrlv` disassembly and operation naming. The existing DSLLV decoder test now expects implemented DSRLV and verifies that DROTRV remains valid-but-unsupported.

The new `test_mips64_dsrlv` target executes encoded instructions and passed **5,618 checks**. Coverage includes the exact Linux word and field decode; counts 0, 1, 7, 15, 31, 32, 47, and 63; count-register values 64, 65, and 127; ignored high count bits; zero, all-one, high-bit, low-bit, and mixed sources; zero fill; no sign extension or 32-bit truncation; source/destination aliases; `$zero` reads and writes; little- and big-endian fetch; normal retirement and Count/PC; taken and not-taken delay slots; GPR, CP0, HI/LO, memory, TLB, and reservation preservation; DSRLV/DROTRV/reserved decoding; CPU serialization; production VM execution; and transactional snapshot rejection.

The focused suite passes exact shift examples including `0x8000000000000000 >> 1 = 0x4000000000000000`, `0x8000000000000000 >> 63 = 1`, and `0xFFFFFFFFFFFFFFFF >> 32 = 0x00000000FFFFFFFF`.

Fresh neighboring focused results:

| Suite | Checks passed |
|---|---:|
| DSLLV | 5,216 |
| DSRL32 | 2,095 |
| SLLV | 5,065 |
| DEXT | 4,357 |

## 7. Snapshot and replay evidence

No serialization or snapshot version changed. The authentic preinstruction snapshot is valid `GXM64SNP` format v3 with CPU component v6 (1,001-byte architectural state), memory v2, UART v4, and IRQ routing v1.

Synthetic full-VM tests capture immediately before DSRLV, execute, restore, and compare the replayed result and full state. They cover both endiannesses and an armed delay slot for both taken and not-taken branches, including restoration before the branch and at the armed instruction. A corrupted full snapshot is rejected transactionally; rejection changes no captured live state or virtual time.

At the authentic Linux frontier, the full VM was captured immediately before DSRLV, restored, and replayed through DSRLV. The immediate replay matched CPU state and RAM/reservation state. The bounded continuation was then run twice from the same captured pre-DSRLV snapshot. The terminal snapshots matched CPU and execution counters, RAM bytes and reservation generations, UART state, guest-visible console bytes/sequence, FPGA state, IRQ routing, and scheduler counters, as well as stop reason and frontier. Snapshot restore itself did not execute guest code or advance virtual time.

## 8. Bounded authentic Linux continuation and next frontier

The runner allowed up to 1,000,000 attempted continuation steps and stopped earlier at a valid but unsupported SPECIAL instruction. The complete bounded trace records every one of the 500 retired instructions after DSRLV. Those 500 retirements visited 420 distinct PCs, showing forward execution through repeated paths; the next instruction was classified before execution.

| Observation | Value |
|---|---|
| Total retired from reset | 152,099 |
| Retired since Linux ELF entry (17 ROM instructions preceded entry) | 152,082 |
| Retired beyond the former DSRLV frontier | 501, including DSRLV |
| Instructions retired after DSRLV | 500 |
| Attempts including next-frontier classification | 152,100 |
| Next PC VA / fetch PA | `0xFFFFFFFF8090DDA4` / `0x00090DDA4` |
| Raw word | `0x00072C3C` |
| Independent decode | `DSLL32 $5,$7,0x10` (LLVM 18.1.8 `llvm-objdump`) |
| Fields / effective DSLL32 count | opcode 0, `rs=0`, `rt=7`, `rd=5`, `sa=16`, funct `0x3C`; 48 |
| `$a3` source / old `$a1` | `0x04FFFFFF000AFFFF` / `0xFFFFFFFF80A6BD38` |
| Stop reason | unsupported instruction, classified before execution |
| Count / VM cycles | 152,099 / 152,099 |
| Status / Cause / EPC / BadVAddr | `0x00400080 / 0 / 0 / 0` |
| Delay armed / valid TLB entries | no / 0 |
| Architectural exception | none |
| UART TX / RX FIFO / console bytes | 0 / 0 / 0 |
| VM state at stop | stopped by the bounded runner |

There is no per-access MMIO counter in this runner, so no claim is made that the continuation performed no MMIO accesses. UART transmit and receive counts and guest-visible console output were zero. The guest did not reach a verified Linux boot milestone.

## 9. Regression and production linkage

- Pre-implementation inherited MIPS64 CTest: **49/49 passed**.
- Post-implementation MIPS64 CTest: **50/50 passed**, including `Mips64DsrlvTests`.
- MIPS64 assertion total: **32,203 passed** (the accepted Phase21 total of 26,582, plus three checks added to the updated DSLLV suite and 5,618 DSRLV checks).
- Selected cross-ISA regressions: **6/6 passed** — `BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`, and `RiscVDecoderSmokeTest`.
- The explicit production/affected target build completed all **350/350** Ninja actions; the DSRLV target rebuilt successfully after the final continuation-trace change.
- `build/bin/ia64emu.exe` contains `Mips64Cpu::executeDecoded`, `step`, `disassemble`, and `opName`, plus `dsrlv` and `dsllv` strings.

The historical separate limitations remain: the IA-64 NaT assertion at `tests/test_isa_plugin.cpp:1551` and six unavailable legacy CTest executables. No IA-64 implementation or marker was changed.

## 10. Trace integrity

The new trace is `docs/artifacts/openwrt_malta_be64_dsrlv_phase22.jsonl`: **763 valid JSONL records**, including 256 prefix instructions, gap/prestate/environment/retirement/snapshot records, 500 individual continuation retirements, continuation summary, and next-frontier record. SHA-256:

`a9a82d77f56e59891a4880905aa5a56a10d0e4cc2f7e0539518540bab41e74e3`

The first 256 records match the Phase21 trace byte-for-byte. The Phase21 trace remains 264 records with its original SHA-256. The authenticated kernel hash also remains unchanged. No accepted guest image or previous accepted trace was overwritten.

## 11. Phase marker, commit, and next scope

`.phase-mips64` now records `last_completed=MIPS64-22` and `next_expected=MIPS64-23`. The IA-64 `.phase` marker remains `last_completed=IA64-POST-MADT-6`, `next_expected=IA64-POST-MADT-7`.

The local MIPS64-22 commit SHA and final branch/upstream divergence are reported with the delivery. No push was made.

Recommended MIPS64-23 scope: independently qualify the captured `DSLL32` at `0xFFFFFFFF8090DDA4`, including its live source and destination state, implement only if architectural decoding confirms the observed word, add encoded and replay tests, then continue the same authenticated kernel to its next genuine frontier. Keep other shift/rotate instructions outside scope unless the authentic stream requires them.
