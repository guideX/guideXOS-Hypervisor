# MIPS64-27: SLTI Signed Comparison and OpenWrt Linux Continuation

## Acceptance result

**Outcome A — complete.** The Phase 26 Linux frontier was reproduced in the production Malta VM, full-width signed `SLTI` was implemented and qualified, the authentic instruction retired with its independently calculated result, and Linux continued to a reproducible `MFC0 $4, $15, 0` execution fault. Linux boot completion remains unverified.

## 1. Repository preflight and Git state

- Checkout: `D:\dev\guideXOS_Hypervisor_MIPS64`, branch `MIPS64`.
- Starting HEAD: `23ecea8e55cc77d87d5d7967372032357befd811`, the requested Phase 26 base. Starting divergence from `origin/MIPS64`: 0 ahead, 0 behind.
- Tracked, untracked, and ignored state was inspected before editing. Tracked and untracked files were clean. Existing ignored state included `build/` and `tests/fixtures/mips64_elf/.objects/`; it was preserved.
- At start, `.phase-mips64` recorded `last_completed=MIPS64-26` and `next_expected=MIPS64-27`. The separate IA-64 `.phase` marker was read and left unchanged.
- Existing branch and worktree topology was retained. No push was made.
- `E:\CodexArtifacts\guideXOS_Hypervisor` was inspected. It contained no MIPS64-specific artifact directory; the accepted kernel was already present under ignored `build/mips64-12-artifacts/`.
- No `AGENTS.md` was present in the checkout.

The Phase 14 SLTIU report and Phase 26 SYNC report were reviewed. Historical traces were left unchanged:

| Artifact | SHA-256 |
|---|---|
| Phase 14 SLTIU trace | `f3748551aa3d53f8f18a66cda6cb700b2a47f9799fc846cf216f1f10deb693e0` |
| Phase 25 DEXTM trace | `222593af3cf8e35b2b415190daf3bf1a0b9c00d90eebb03b8d86f6e8752c0c2f` |
| Phase 26 SYNC trace | `5b7627802f8365e080e9edff2565818a8696a63b5bd59791452c435625645397` |

The accepted Phase 26 trace has 270 records. Before implementation, all 54 inherited MIPS64 CTest suites passed.

## 2. Authenticated kernel and VM

The unchanged OpenWrt 24.10.4 Malta BE64 kernel identifies as Linux 6.6.110. Its ELF is 11,013,776 bytes, with SHA-256 `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc` and entry `0xffffffff80917670`. The production VM used 256 MiB of guest RAM, big-endian mode, and the accepted Malta ROM/PROM handoff with command line `console=ttyS0,38400n8`.

The kernel hash matched the previously accepted image. No guest bytes were patched, no register was forced, and the ROM/PROM configuration was not substituted.

## 3. Reproduced frontier and authentic operands

The production VM re-executed the first 256 instruction records and matched both the accepted Phase 25 and Phase 26 records byte-for-byte. At the prior Phase 26 SYNC frontier, the full GPR prestate was compared with the accepted Phase 26 record, the SYNC retirement fields were checked, and the six following `continuation_instruction` records matched Phase 26 byte-for-byte. That overlap returned to the SLTI frontier without divergence.

Before adding the decoder support, the exact Linux instruction was fetched directly from guest memory and the old decoder reported `UnsupportedInstruction`. The captured instruction and operands were:

| Field | Captured value |
|---|---|
| Guest PC | `0xffffffff8091abc4` |
| Guest physical fetch address | `0x00091abc4` |
| Raw word | `0x2a220000` |
| Opcode | `0x0a` — SLTI |
| `rs` / source | `17` / `$s1` |
| `rt` / destination | `2` / `$v0` |
| Immediate bits | `0x0000` |
| Sign-extended immediate | `0` |
| `$s1` bits and signed value | `0x0000000000000000` / `0` |
| Previous `$v0` | `0x0000000000000000` |
| Independently expected result | `0` |
| CP0 Status / Cause / EPC / BadVAddr | `0x00400080` / `0` / `0` / `0` |
| Count / VM cycles / retired before SLTI | `157787` / `157787` / `157787` |
| Delay slot armed | No |

Opcode and field extraction from the raw word independently gives `0x0a`, `rs=17`, `rt=2`, and immediate `0`. Since the captured source is signed 64-bit zero, `0 < 0` is false. This result was calculated from the captured register, not assumed from the Linux instruction or injected by the host.

## 4. SLTI implementation and architectural distinction

The implementation follows the MIPS64 Architecture for Programmers, Volume II, Revision 2.50, SLTI entry (printed page 337): [Volume II, Revision 2.50 PDF](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf). The operation sign-extends the 16-bit immediate, compares the complete 64-bit source as signed, writes exactly zero or one, and does not raise an integer-overflow exception.

Changes are confined to the CPU implementation and qualification files:

- [`Mips64Cpu.h`](../include/mips64/Mips64Cpu.h): appended `Mips64Op::Slti`, preserving prior enum values.
- [`Mips64Cpu.cpp`](../src/mips64/Mips64Cpu.cpp): recognizes opcode `0x0a`; uses `std::bit_cast<int64_t>` for the source and signed-16-to-64 conversion for the immediate; reads before writing to support `rs == rt`; adds `slti` naming and disassembly.
- [`test_mips64_sltiu.cpp`](../tests/test_mips64_sltiu.cpp): replaces the former unsupported-SLTI assertion with decode, naming, disassembly, and execution checks proving signed SLTI and unsigned SLTIU remain distinct.
- [`test_mips64_slti.cpp`](../tests/test_mips64_slti.cpp) and [`CMakeLists.txt`](../CMakeLists.txt): add the focused architectural suite and authentic production-VM replay harness.

SLTIU remains unchanged: it sign-extends the same immediate but compares it as unsigned. For source `0xffffffffffffffff` and immediate `33`, SLTI returns 1 (`-1 < 33`) and SLTIU returns 0 (`UINT64_MAX < 33` is false).

## 5. Focused architecture and snapshot qualification

`test_mips64_slti` reported **1,213 passed, 0 failed**. Its independently calculated signed cases cover zero, positive and negative values, `INT64_MIN` and `INT64_MAX`, values around positive and negative thresholds, immediates `0`, `1`, `-1`, `-32768`, and `+32767`, and values that expose accidental 32-bit truncation. Encoded execution is exercised in both big- and little-endian modes.

The focused checks also cover exact Linux decoding, immediate sign extension, full-width 0/1 results, source and destination `$zero`, `rs == rt` ordering, normal PC and Count progression, no overflow exception, no unrelated GPR/CP0/HI/LO/TLB/memory/reservation changes, and taken and not-taken branch delay slots. CPU serialization is round-tripped.

The synthetic full-VM scenarios capture, execute, restore, and replay SLTI and SLTIU signed/unsigned pairs, register aliasing, and an armed delay slot. They compare architectural state, retirement, PC, cycles, branch target, and delay completion. Invalid snapshot data is rejected transactionally without changing the live VM. The separate SLTIU executable reported **4,001 passed, 0 failed**.

No snapshot format changed. Qualification retained full VM `GXM64SNP` v3, CPU v6 (1,001 architectural-state bytes), memory v2, UART v4, and IRQ routing v1.

## 6. Authentic SLTI retirement and Linux continuation

The production VM captured a valid full snapshot immediately before the authentic SLTI. The CPU decoded and retired the original guest word. It observed `$v0=0`, advanced PC to `0xffffffff8091abc8`, left the delay slot clear, and advanced Count, cycles, and retirement from 157,787 to 157,788. The complete expected GPR state matched; CP0 state changed only by normal Count progression. HI/LO, RAM, reservation generations, TLB, UART, and console state remained unchanged. No overflow or architectural exception occurred.

Execution then continued through **343 additional retired guest instructions** after SLTI. Across the old frontier, that is **344 additional retired instructions including SLTI**, for 158,131 total retired and 158,114 retired since the Linux ELF entry. The return sequence restores saved registers and reaches PC `0xffffffff80184630`; its `jal 0x187618` delay slot at `0xffffffff80184634` executes `or $17, $2, $0`, copying the SLTI-produced `$v0` return value into `$s1`. The target proceeds through `di`, `andi`, and further initialization code. No function name or boot milestone is inferred from stripped addresses.

The next instruction is at guest PC `0xffffffff8010bf3c`, PA `0x00010bf3c`, raw word `0x40047800`, disassembled `mfc0 $4, $15, 0`. A preflight execution attempt ended the VM in `VM_ERROR` with internal `Cp0Violation`; the current `readCp0()` path does not support this CP0 register selection. That failed attempt advanced Count to 158,132 but did not advance VM cycles or guest retirement, which remained 158,131. It did not set an architectural exception: Cause, EPC, and BadVAddr remain zero. For replay, the VM restores the captured pre-SLTI snapshot and stops cleanly at the preflight-confirmed instruction boundary, before executing the failing MFC0. Thus the terminal snapshot is valid at Count/cycles/retirement 158,131.

At the terminal boundary, Status is `0x00400080`, no branch delay is armed, there are zero valid TLB entries, and there is no active reservation. UART TX, UART RX FIFO, and console output are all zero bytes. MMIO observations were not instrumented and are not claimed. Linux boot completion is unverified.

The authentic continuation was bounded by one million attempts and stopped at the earlier reproducible execution fault. The production pre-SLTI snapshot was restored and the SLTI plus continuation replayed; CPU, physical RAM, reservation generations, devices, UART/console, scheduler state, virtual cycles, trace window, and final stop matched.

## 7. Trace and replay integrity

The new trace is [`openwrt_malta_be64_slti_phase27.jsonl`](artifacts/openwrt_malta_be64_slti_phase27.jsonl), with **369 valid JSONL records** and SHA-256 `B4A2B3343CD9C4810CE5311531546272EF4C1E9ACF923A43B9E409F6063594F9`. It records the environment identity, exact overlap comparisons, full authentic SLTI prestate, observed retirement, invalid-restore rejection, bounded continuation, next frontier, and deterministic replay result.

The 256-record initial instruction prefix is identical to both accepted Phase 25 and Phase 26 traces. For the Phase 26 SYNC overlap, the prestate record includes all 32 GPRs and matched the accepted state; SYNC retirement state matched; each of the next six instruction records matched byte-for-byte. The original Phase 14, Phase 25, and Phase 26 traces were hash-checked and remain unchanged.

## 8. Regression and production linkage

- MIPS64 CTest: **55/55 passed**, covering the 54 inherited suites plus the new SLTI suite.
- Selected cross-ISA regressions: **6/6 passed** (`BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`, and `RiscVDecoderSmokeTest`). The combined selected run was **61/61 passed**.
- Reported MIPS64 assertion total: **425,664**, computed from the Phase 26 total of 424,442, the SLTIU increase from 3,992 to 4,001 (+9), and 1,213 SLTI checks.
- Affected MIPS64 tests and production `ia64emu.exe` were built. The linked production binary contains `decodeInstruction`, `Mips64Cpu::executeDecoded`, `Mips64Cpu::disassemble`, and `Mips64Cpu::opName`; binary strings include `slti`.

No unrelated IA-64 source or marker changed. Previously documented unrelated limitations remain separate: the IA-64 NaT assertion in the broader ISA-plugin test, six unavailable legacy CTest executables, and historical aggregate-build limitations.

## 9. Phase tracking, limitations, and next scope

On acceptance, `.phase-mips64` is advanced to `last_completed=MIPS64-27` and `next_expected=MIPS64-28`. The IA-64 `.phase` remains unchanged. No Linux boot is claimed, and no unrelated instruction, device, firmware service, or snapshot format was added.

Recommended MIPS64-28 scope: qualify the exact `MFC0 $4, $15, 0` access as a CP0 PRId read under the observed Status and privilege state. Establish the register-selection semantics and distinguish a valid architectural read from an architectural exception before extending CP0 support. Keep the investigation limited to this observed frontier.
