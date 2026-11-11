# MIPS64-24 — DSRA32 Qualification and OpenWrt Linux Continuation

**Outcome A — complete.** The authenticated DSRA32 frontier was reproduced before implementation, independently decoded, implemented as a full-width signed arithmetic shift, retired by the production Malta VM, and replayed deterministically from full-VM snapshots. The unchanged Linux stream retired 1,787 more instructions before reaching a valid but unsupported SPECIAL3 instruction. Linux boot completion is not claimed.

## 1. Preflight and repository state

- Checkout: `D:\dev\guideXOS_Hypervisor_MIPS64`; branch: `MIPS64`.
- Starting HEAD: `24465c544668085b4031776ca331b9ee454642a9`, the expected MIPS64-23 state.
- Starting tracked and untracked worktree: clean. The upstream tracking ref was `30993573ee36bcff20eac1984f5e41427290d1c5`; local divergence was two commits ahead and zero behind. No branch, worktree, or Git topology operation was performed, and nothing was pushed.
- `.phase-mips64` recorded MIPS64-23 complete and MIPS64-24 next. The separate IA-64 `.phase` marker and IA-64 source were preserved.
- MIPS64-22 and MIPS64-23 reports, the accepted Phase 23 trace, fixed and variable shift implementations, decoder reserved-field handling, encoded test patterns, Malta VM replay, and full-VM snapshot support were inspected.
- The artifact directory `E:\CodexArtifacts\guideXOS_Hypervisor` and the documented repository artifact locations were inspected. The existing authenticated ELF under ignored `build/mips64-12-artifacts` was reused; no accepted kernel or trace was overwritten.
- All **51 inherited MIPS64 CTest suites passed before implementation**.

## 2. Authenticated kernel and VM configuration

| Property | Value |
|---|---|
| Distribution / platform | OpenWrt 24.10.4 / Malta BE64 |
| Linux version | 6.6.110 |
| ELF | `build/mips64-12-artifacts/openwrt-24.10.4-malta-be64-vmlinux.elf` |
| Size | 11,013,776 bytes |
| SHA-256 | `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc` |
| ELF entry | `0xFFFFFFFF80917670` |
| RAM / endianness | 256 MiB / big-endian |
| PROM command line | `console=ttyS0,38400n8` |
| Malta ROM | Existing 4 MiB reset/PROM image |

The ELF hash was checked before and after the production run and remained unchanged. No kernel patch, instruction skip, or host register injection was used.

## 3. Phase 23 baseline and pre-change frontier reproduction

Phase 23 accepted 152,100 retired instructions and stopped at the unsupported DSRA32 at `0xFFFFFFFF8090DDA8`, immediately after DSLL32 at `0xFFFFFFFF8090DDA4`. The accepted Phase 23 trace has 263 records and SHA-256 `973ae721882989b9fae37a01438df648a22c23608b6ac503dc64c3c2e864d67d`.

Before changing the decoder or executor, the existing production VM replayed the unchanged ELF and generated `build/mips64-24-preflight/dsra32-before-implementation.jsonl`. That pre-change replay has the same SHA-256 as the accepted Phase 23 trace, including the raw unsupported DSRA32 word and live source register. It confirms the original ROM/PROM handoff, 256 MiB RAM, and instruction prefix were reproduced exactly.

## 4. DSRA32 encoding and architectural semantics

The [MIPS64 Architecture for Programmers, Volume II, Revision 2.50, DSRA32 entry, p. 169](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf) defines DSRA32 as an arithmetic right shift of a doubleword by `sa + 32`, with bit 63 replicated into vacated high bits. The independent [GNU Binutils MIPS simulator definition](https://gnu.googlesource.com/binutils-gdb/+/37f7f684de0e53c8add5ff43d73082bf98985321/sim/mips/mips.igen) encodes the canonical `rs=0`, function `0x3F` form and uses a 32-plus-shift count.

The exact fetched word `0x00052C3F` decodes as:

| Field | Value |
|---|---|
| Primary opcode | `0x00` — SPECIAL |
| `rs` | `0` — canonical reserved zero |
| `rt` | `5` — `$a1`, source |
| `rd` | `5` — `$a1`, destination |
| `sa` | `16` |
| Function | `0x3F` — DSRA32 |
| Assembly | `DSRA32 $a1,$a1,16` |
| Effective shift | `16 + 32 = 48` |

LLVM 18.1.8 independently disassembled the authenticated ELF bytes at the frontier as `dsra32 $5, $5, 0x10`. The implementation reads the complete `uint64_t` source, shifts it unsigned by 32–63, and fills high bits with ones only when original bit 63 is set. This avoids implementation-defined signed conversion and signed-shift behavior. It reads the source before writing `rd`, preserves the existing `$zero` helper behavior, and has no exception, memory, CP0, HI/LO, TLB, or reservation side effect.

Only canonical `rs=0` DSRA32 is accepted. Nonzero `rs` encodings raise the established Reserved Instruction exception; the function is not broadened to unrelated SPECIAL instructions.

## 5. Actual Linux prestate and retirement

The VM replayed the prefix from reset and retired the preceding DSLL32. At `0xFFFFFFFF8090DDA4`, guest memory contained `0x00072C3C` (`DSLL32 $a1,$a3,16`); live `$a1` after that instruction was `0xFFFF000000000000`. At the DSRA32 frontier:

| Observation | Value |
|---|---|
| Retired / Count / VM cycles before DSRA32 | 152,100 / 152,100 / 152,100 |
| PC VA / fetch PA | `0xFFFFFFFF8090DDA8` / `0x00090DDA8` |
| Raw word / operation | `0x00052C3F` / `DSRA32 $a1,$a1,16` |
| `$a1` source / previous destination | `0xFFFF000000000000` / `0xFFFF000000000000` |
| Delay slot | Clear; no pending branch |
| Status / Cause / EPC / BadVAddr | `0x00400080 / 0 / 0 / 0` |
| HI / LO / valid TLB entries | `0 / 0 / 0` |
| LL/SC reservation | Invalid |

An independent bit-by-bit calculation shifts the full 64-bit negative source right by 48 and fills the upper vacated bits from its one sign bit. The expected result is `0xFFFFFFFFFFFFFFFF` (signed `-1`). The result is not the logical-shift value `0x000000000000FFFF`.

DSRA32 retired once through the normal VM path. `$a1` became `0xFFFFFFFFFFFFFFFF`; PC advanced to `0xFFFFFFFF8090DDAC`; the delay state remained clear. Count, VM cycles, and retired instructions advanced once to 152,101. The other GPRs, CP0 state except Count, HI/LO, memory, and reservation state were preserved. No arithmetic exception occurred.

## 6. Implementation and focused qualification

The in-scope implementation changes are in `include/mips64/Mips64Cpu.h`, `src/mips64/Mips64Cpu.cpp`, `CMakeLists.txt`, and `tests/test_mips64_dsra32.cpp`. The existing `tests/test_mips64_dsll32.cpp` neighboring classification now expects DSRA32 to be implemented.

- Appended `Mips64Op::Dsra32`, preserving prior enum values.
- Added SPECIAL function `0x3F` recognition with canonical `rs=0` validation.
- Added full-width, portable sign-fill execution and reused normal GPR write and retirement handling.
- Added `dsra32` operation naming and `dsra32 rd,rt,sa` disassembly.
- Added the `test_mips64_dsra32` executable and `Mips64Dsra32Tests` CTest entry.

The focused target passed **4,025 checks**. It executes encoded instructions and covers the exact Linux word; SPECIAL/function and `rs/rt/rd/sa` fields; malformed reserved encodings; effective shifts 32, 33, 39, 47, 48, 55, and 63; zero, one, all ones, high-bit, positive, and negative operands; sign-fill and retained low bits; the authenticated result versus logical shift; aliasing and both `$zero` cases; little- and big-endian instruction fetch; PC, Count, and retirement; taken and not-taken branch delay slots; a reserved-instruction delay-slot fault; unrelated GPR, CP0, HI/LO, memory, and reservation preservation; distinction from DSRA, DSRL32, DSRLV, and DSLL32; operation naming and disassembly; CPU serialization; production VM execution; and full-VM snapshot restore/rejection.

## 7. Neighboring regressions and production linkage

- Pre-change inherited MIPS64 CTest: **51/51 passed**.
- Post-change MIPS64 CTest: **52/52 passed**, including the new DSRA32 suite.
- Selected cross-ISA CTest: **6/6 passed** — `BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`, and `RiscVDecoderSmokeTest`.
- Assertion accounting: accepted inherited baseline **32,203**; updated DSLL32 focused target **385,326**; DSRA32 focused target **4,025**; **421,554 total reported assertions**.
- Fresh neighboring focused checks passed: DSLL32 **385,326**, DSRL32 **2,095**, DSLLV **5,216**, DSRLV **5,618**, SLLV **5,065**, DEXT **4,357**, and SLTIU **3,992**. These targeted results overlap the aggregate assertion accounting above.
- Production and affected targets completed **276 Ninja actions** across the initial production/new-test build, targeted MIPS64 regression rebuilds, and the updated DSLL32 neighboring test rebuild.
- `build/bin/ia64emu.exe` contains linked `Mips64Cpu::executeDecoded`, `Mips64Cpu::disassemble`, and `Mips64Cpu::opName` symbols, the `dsra32` mnemonic, and the DSRA32 reserved-field diagnostic.

Historical separate limitations remain unchanged: the IA-64 NaT assertion at `tests/test_isa_plugin.cpp:1551`, six unavailable legacy CTest executables, and unrelated aggregate all-target build issues. No IA-64 source or marker was changed.

## 8. Delay-slot behavior

The authenticated Linux DSRA32 was not in an armed delay slot. It advanced to PC+4 with the delay state clear. Encoded tests separately exercise DSRA32 as the delay-slot instruction after taken and not-taken branches, restore a full-VM snapshot with the slot armed, and verify identical result, target completion, PC, delay state, and counters. Malformed reserved DSRA32 in a delay slot retains Cause.BD/EPC behavior.

## 9. Snapshot versions and deterministic replay

No architectural state was added and no serialization version changed:

- Full VM: `GXM64SNP` v3.
- CPU: v6, 1,001 bytes.
- Memory: v2.
- UART: v4.
- IRQ routing: v1.

Synthetic full-VM tests snapshot immediately before encoded DSRA32, execute, restore, and re-execute with matching CPU state, PC, Count, VM cycles, memory, devices, scheduler, and result. They also cover an armed branch delay slot and transactional rejection of a corrupted snapshot without changing the live state or advancing virtual time.

The authentic production VM snapshot was captured immediately before the Linux DSRA32. The first retirement and replay matched. The bounded continuation was run twice from the same preinstruction snapshot; CPU state, RAM bytes and reservation generations, UART, FPGA, console, scheduler, counters, and stop reason/frontier matched. Snapshot restore did not execute guest code or advance virtual cycles.

## 10. Bounded authentic Linux continuation and next frontier

The runner permitted up to 1,000,000 continuation attempts and stopped earlier at a valid but unsupported instruction after forward progress through 168 distinct PCs.

| Observation | Value |
|---|---|
| Total retired from reset | 153,888 |
| Retired since Linux ELF entry | 153,871 |
| Retired beyond former DSRA32 frontier, including DSRA32 | 1,788 |
| Instructions retired after DSRA32 | 1,787 |
| Attempts including next-frontier classification | 153,889 |
| Next PC VA / fetch PA | `0xFFFFFFFF80188804` / `0x000188804` |
| Raw word | `0x7E31E801` |
| Independent field decode | opcode `0x1F`, `rs=17`, `rt=17`, `rd=29`, `sa=0`, function `1` |
| LLVM 18.1.8 disassembly | `dext $17, $17, 0x0, 0x3e` |
| Emulator classification | Valid but unsupported SPECIAL3 instruction outside the implemented DEXT subset |
| Stop reason | Unsupported instruction, classified before execution |
| Count / VM cycles | 153,888 / 153,888 |
| Status / Cause / EPC / BadVAddr | `0x00400080 / 0 / 0 / 0` |
| Delay slot / valid TLB entries | Clear / 0 |
| UART TX / RX FIFO / console bytes | 0 / 0 / 0 |

This runner does not collect a general MMIO access count, so no MMIO total is claimed. No architectural exception occurred at the stop. The VM was stopped by the bounded runner at the unsupported frontier. No Linux boot milestone is claimed.

## 11. Trace integrity, known limits, and MIPS64-25 recommendation

The bounded Phase 24 trace is [`docs/artifacts/openwrt_malta_be64_dsra32_phase24.jsonl`](artifacts/openwrt_malta_be64_dsra32_phase24.jsonl). It contains **2,051 valid JSONL records** and has SHA-256 `af83f13c4b9e8593d1f757a8a656a2ea987b21ab21c50677255b9092c0613037`. Its first 256 records match the accepted Phase 23 prefix and the Phase 22 trace byte-for-byte. The Phase 23 trace remains SHA-256 `973ae721882989b9fae37a01438df648a22c23608b6ac503dc64c3c2e864d67d`; the Phase 22 trace remains SHA-256 `a9a82d77f56e59891a4880905aa5a56a10d0e4cc2f7e0539518540bab41e74e3`. The authenticated ELF hash is unchanged.

No historical report or trace was rewritten; no kernel, snapshot, guest image, or uncontrolled log was committed. Full Linux boot and general MMIO access counts remain unverified. The prior IA-64 and unavailable legacy-test limitations are separate and unchanged.

Recommended MIPS64-25 scope: independently qualify the captured SPECIAL3 frontier at `0xFFFFFFFF80188804`, including whether the emitted `dext` encoding is DEXTM or another format variant, then implement only the authenticated operation and continue the same unchanged kernel. Do not add speculative instruction or Malta platform support.

The phase changes and this evidence are committed locally on `MIPS64`; the branch remains ahead of its unchanged upstream tracking ref, and no push was made.
