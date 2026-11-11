# MIPS64-23 — DSLL32 Qualification and OpenWrt Linux Continuation

**Outcome A — complete.** The authentic DSLL32 at `0xFFFFFFFF8090DDA4` was independently decoded, implemented, retired through the production Malta VM, and replayed from a full-VM snapshot. The unchanged kernel then reached a valid but unsupported DSRA32 one instruction later. Linux boot completion remains unverified.

## 1. Repository preflight and Git state

- Checkout: `D:\dev\guideXOS_Hypervisor_MIPS64`; branch: `MIPS64`.
- Starting HEAD: `8a215582e832e9bd21bf93927f7df5134088f8b7`, the required MIPS64-22 commit.
- At preflight, `origin/MIPS64` was `30993573ee36bcff20eac1984f5e41427290d1c5`; local divergence was one commit ahead and zero behind.
- The initial tracked and untracked worktree was clean. Ignored `build/` and `tests/fixtures/mips64_elf/.objects/` were inspected and preserved. There was no repository `.codex_tmp` directory.
- `.phase-mips64` recorded `last_completed=MIPS64-22` and `next_expected=MIPS64-23`. It now records MIPS64-23 complete and MIPS64-24 next. The IA-64 `.phase` marker and IA-64 source were not changed.
- MIPS64-20 through MIPS64-22 reports, the accepted Phase 22 trace, DSRL32/DSLL/DSLLV/DSRLV code and tests, CPU serialization, full-VM snapshot handling, and production Linux replay harness were inspected.
- `E:\CodexArtifacts\guideXOS_Hypervisor` had no MIPS64-specific artifact directory. The existing ignored local kernel artifact was reused; no kernel, snapshot, or accepted trace was overwritten.
- Before implementation, all **50 inherited MIPS64 CTest suites passed**.

No branch/worktree creation, reset, rebase, merge, cherry-pick, stash, clean, topology change, or push was performed.

## 2. Authenticated kernel and Malta configuration

The accepted kernel at `build/mips64-12-artifacts/openwrt-24.10.4-malta-be64-vmlinux.elf` was reused without modification.

| Property | Value |
|---|---|
| Distribution / platform | OpenWrt 24.10.4 / Malta BE64 |
| Linux version | 6.6.110 |
| ELF size | 11,013,776 bytes |
| SHA-256 | `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc` |
| Entry point | `0xFFFFFFFF80917670` |
| Guest RAM | 256 MiB, big-endian |
| PROM command line | `console=ttyS0,38400n8` |
| Malta ROM | Existing 4 MiB reset/PROM image |

The final kernel hash remained unchanged. The production VirtualMachine, reset ROM, PROM arguments, and RAM configuration were retained.

## 3. Phase 22 acceptance baseline and replay integrity

The accepted Phase 22 report records 152,099 retired instructions, 501 retirements beyond its DSRLV frontier including DSRLV, 420 distinct continuation PCs, and the next unsupported instruction at `0xFFFFFFFF8090DDA4`. It contains 763 JSONL records and retains SHA-256 `a9a82d77f56e59891a4880905aa5a56a10d0e4cc2f7e0539518540bab41e74e3`.

Before implementation, the production DSRLV harness replayed the unchanged kernel to the same DSLL32 boundary. Its generated trace was byte-for-byte identical to the accepted Phase 22 trace, including the recorded DSRLV retirement and continuation. The replay observed the exact guest PC and raw word; the current decoder classified it as valid-but-unsupported before the Phase 23 change.

The accepted Phase 21 trace remains SHA-256 `5dd2aed393017e0f1b2d41aa7b1f834cd29643f9f45fd1a5fa142c1719681602`. The first 256 instruction records in the accepted Phase 21 trace, accepted Phase 22 trace, the fresh Phase 22 replay, and the new Phase 23 trace compare byte-for-byte. Only this overlapping prefix is claimed identical.

## 4. DSLL32 encoding and independent classification

The exact big-endian word at the authenticated frontier is `0x00072C3C`, with bytes `00 07 2C 3C`:

| Field | Bits | Value |
|---|---:|---:|
| Primary opcode | 31–26 | `0x00` — SPECIAL |
| `rs` | 25–21 | `0` — canonical reserved zero |
| `rt` | 20–16 | `7` — `$a3` |
| `rd` | 15–11 | `5` — `$a1` |
| `sa` | 10–6 | `16` |
| Function | 5–0 | `0x3C` — DSLL32 |

The independent bit-field decode gives `DSLL32 $a1,$a3,16`. LLVM 18.1.8 independently disassembled the authenticated ELF bytes at the reported virtual address as `dsll32 $5, $7, 0x10`. The primary architecture reference, [MIPS64 Architecture for Programmers, Volume II, Revision 2.50, DSLL32, p. 166](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf), defines the operation as `GPR[rd] = GPR[rt] << (sa + 32)` and shows `rs=0` in the encoding.

The implementation accepts only canonical `rs=0` DSLL32 encodings. Other `rs` values for function `0x3C` are reserved and raise the existing Reserved Instruction exception. Adjacent valid encodings remain separate: DROTR32, DSRA32, and other implemented/unimplemented SPECIAL functions are not accepted as DSLL32.

## 5. Actual Linux operands and result

The production VM reached the frontier from the unchanged reset-ROM/PROM flow after 152,099 retirements. Before executing the instruction it observed:

| Observation | Value |
|---|---|
| Guest PC / physical fetch | `0xFFFFFFFF8090DDA4` / `0x00090DDA4` |
| Raw word / decoded operation | `0x00072C3C` / DSLL32 |
| `$a3` source | `0x04FFFFFF000AFFFF` |
| Previous `$a1` | `0xFFFFFFFF80A6BD38` |
| `sa` / effective shift | 16 / 48 |
| Delay slot armed | No |
| CP0 Count / VM cycles | 152,099 / 152,099 |
| Status / Cause / EPC / BadVAddr | `0x00400080 / 0 / 0 / 0` |
| HI / LO / valid TLB entries | `0 / 0 / 0` |
| LL/SC reservation | Invalid |

The independent expected result is computed from the observed full 64-bit source. With a shift of 48, only source bits 0–15 can remain: `$a3 & 0xFFFF = 0xFFFF`, therefore `0xFFFF << 48 = 0xFFFF000000000000`. The emulator produced exactly `0xFFFF000000000000` in `$a1`.

DSLL32 retired normally through the production VM. PC advanced by four to `0xFFFFFFFF8090DDA8`; the delay state remained clear. CP0 Count, VM cycles, and retired instructions each advanced once to 152,100. No other GPR, CP0 register (apart from Count), HI/LO, memory, TLB entry, or reservation state changed. No arithmetic exception was generated.

## 6. Implementation and focused qualification

The code change is limited to the MIPS64 CPU header/source, `CMakeLists.txt`, and `tests/test_mips64_dsll32.cpp`:

- Appended `Mips64Op::Dsll32`, preserving existing enum values.
- Added SPECIAL function `0x3C` decoding with canonical `rs=0` validation.
- Added execution using the full `uint64_t` source, `sa + 32`, an unsigned logical left shift, and the established GPR write helper.
- Added `dsll32` operation naming and R-format disassembly.
- Kept all unrelated functions and unsupported instruction classifications intact.
- Added the `test_mips64_dsll32` CMake/CTest target.

The focused target passed **385,325 checks**. It executes encoded instructions in both little- and big-endian modes and covers the exact Linux word; every `sa` value from 0 through 31 across all one-hot source bits; the required shift values 32, 33, 39, 47, 48, 55, and 63; zero, all-one, mixed, and single-bit operands; discarded high bits and zero fill; register aliases; `$zero`; malformed reserved fields; normal PC/count retirement; delay slots for taken and not-taken branches; operation naming and disassembly; neighboring DSLL/DSLLV/DSRL32/DSRA32 classifications; CPU serialization; production VM execution; full-VM snapshots; and rejection of a corrupted snapshot without changing live VM state.

## 7. Neighboring instruction regressions and production linkage

After the implementation and rebuild, **51/51 MIPS64 CTest suites passed**, comprising all 50 inherited suites and the new DSLL32 suite. The inherited coverage includes DSLL, DSLLV, DSRL/DSRL32/DSRLV, SLL/SLLV, DEXT, SLTIU, TNE, DI/EI/EHB, LL/SC and LLD/SCD, delay slots, CP0 exceptions, timers/UART interrupts, Malta ELF/PROM handoff, and full-VM snapshots.

The selected cross-ISA regressions passed **6/6**: `BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`, and `RiscVDecoderSmokeTest`.

Focused inherited neighboring instruction totals from the postchange run were DSLLV **5,216**, DSRL32 **2,095**, DSRLV **5,618**, SLLV **5,065**, DEXT **4,357**, and SLTIU **3,992** checks passed.

The production `ia64emu.exe` and all affected MIPS64 targets built successfully: **291/291 Ninja actions**. Inspection of the linked production binary found `Mips64Cpu::executeDecoded`, `disassemble`, and `opName`, along with the `dsll32` mnemonic and DSLL32 reserved-field diagnostic. The MIPS64 assertion total is the accepted inherited baseline of 32,203 plus 385,325 new focused checks, or **417,528**.

## 8. Delay-slot observations

The authenticated Linux DSLL32 was not in an armed delay slot. It advanced to PC+4 and left the delay state clear. Encoded tests separately exercise DSLL32 as the delay-slot instruction after both taken and not-taken branches, including target completion and restoration from a full-VM snapshot with the delay slot armed. Malformed reserved DSLL32 encodings retain the established RI Cause.BD/EPC behavior.

## 9. Snapshot versions and deterministic replay

No architectural state was added, so serialization and snapshot versions remain unchanged:

- Full VM: `GXM64SNP` v3.
- CPU: v6, 1,001 serialized bytes.
- Memory: v2.
- UART: v4.
- IRQ routing: v1.

The synthetic full-VM test snapshots immediately before DSLL32, executes it, restores, and re-executes with matching CPU/RAM/device/scheduler state and counters. A corrupted snapshot is rejected transactionally without advancing guest time or modifying the VM. The synthetic delay-slot tests also restore the armed state and reproduce the same shift result, delay-target completion, PC, and counters.

The authentic production VM snapshot was captured at the actual DSLL32 frontier. The first execution and replay matched CPU serialization, instruction/cycle counters, physical RAM and reservation generations, devices, console output/sequence, and scheduler state. Restore itself did not execute guest code or advance virtual cycles. Replaying from the same pre-DSLL32 snapshot through DSLL32 and the bounded continuation reproduced the same terminal VM state and stop reason.

## 10. Authentic Linux continuation and next frontier

DSLL32 retired as instruction 152,100 from reset, or 152,083 retired instructions since the Linux ELF entry (17 reset-ROM instructions preceded entry). It is the only retirement beyond the former DSLL32 frontier; the next instruction was classified before execution, so zero instructions retired after DSLL32.

The next fetched word is `0x00052C3F` at VA `0xFFFFFFFF8090DDA8`, PA `0x00090DDA8`. Its fields are SPECIAL opcode `0`, `rs=0`, `rt=5`, `rd=5`, `sa=16`, function `0x3F`. LLVM 18.1.8 independently identifies it as `DSRA32 $5,$5,0x10`. The emulator retains the valid-but-unsupported classification and stops before execution. DSRA32 was not implemented in this phase.

| Final observation | Value |
|---|---|
| Total retired / Count / VM cycles | 152,100 / 152,100 / 152,100 |
| Retired beyond former DSLL32 frontier | 1, including DSLL32 |
| Retired after DSLL32 | 0 |
| Stop | Valid unsupported DSRA32 at `0xFFFFFFFF8090DDA8` |
| Status / Cause / EPC / BadVAddr | `0x00400080 / 0 / 0 / 0` |
| Delay slot / valid TLB entries | Clear / 0 |
| UART TX / RX FIFO / guest console bytes | 0 / 0 / 0 |

The bounded runner allowed up to 1,000,000 attempts and stopped immediately at the unsupported frontier. It does not collect a general MMIO access counter, so no MMIO total is claimed. Linux boot completion remains unverified.

## 11. Trace identity, known limits, and MIPS64-24 recommendation

The bounded trace is [`docs/artifacts/openwrt_malta_be64_dsll32_phase23.jsonl`](artifacts/openwrt_malta_be64_dsll32_phase23.jsonl). It contains 263 valid JSONL records and has SHA-256 `973ae721882989b9fae37a01438df648a22c23608b6ac503dc64c3c2e864d67d`. It includes the 256-record accepted prefix, the DSLL32 prestate/retirement, both snapshot replay records, continuation summary, and unsupported next-frontier record.

The accepted Phase 22 trace and kernel hashes remain unchanged. No historical report or trace was rewritten. Full Linux boot, MMIO-wide counters, and unimplemented instructions outside this authenticated frontier remain unverified or unsupported. Previously documented unrelated limitations (the IA-64 NaT assertion at `tests/test_isa_plugin.cpp:1551` and six unavailable legacy CTest executables) were not modified or part of this qualification.

Recommended MIPS64-24 scope: independently qualify the authenticated DSRA32 at `0xFFFFFFFF8090DDA8`, capture its live source/destination state and delay status, implement only DSRA32 if confirmed, add encoded and replay tests, and continue the same unchanged kernel to its next genuine frontier. Do not implement DSRAV or other shift/rotate instructions absent an authenticated need.
