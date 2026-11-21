# MIPS64-33 — MOVN and authentic OpenWrt Linux continuation

**Outcome A — complete.** The CPU now implements integer MOVN with full 64-bit
condition and copy semantics. The production Malta VM reproduced the
authenticated OpenWrt instruction, retired it with the value dictated by the
live guest registers, replayed the authentic full-VM continuation, and
identified the next runtime CP0 selector fault. Full Linux boot is not claimed.

## 1. Repository preflight and preserved inputs

- Repository: `D:\dev\guideXOS_Hypervisor_MIPS64`; branch: `MIPS64`.
- Starting HEAD: `33da3bf03abb99e63b4a8d8a2dcef2fa4ce52773`.
- At preflight the branch was **2 commits ahead, 0 behind** `origin/MIPS64`.
  The checkout was treated as authoritative. The tracked and untracked
  worktree was clean. Existing ignored paths included `build/` and
  `tests/fixtures/mips64_elf/.objects/`.
- `.phase-mips64` recorded MIPS64-32 complete and MIPS64-33 next. The separate
  IA-64 `.phase` marker and IA-64 implementation were left unchanged.
- The Phase 31 and 32 reports, accepted traces, SPECIAL/SPECIAL3 decoders,
  GPR read/write helpers, comparison operations, delay-slot retirement,
  CPU/full-VM snapshots, and production Malta replay paths were inspected.
- `E:\CodexArtifacts\guideXOS_Hypervisor` and the documented MIPS64 artifact
  directory were checked. The existing kernel below was reused; no guest image,
  ROM, PROM, accepted trace, or checkpoint was replaced.
- Before implementation, all **60 inherited MIPS64 CTest cases passed**.
- No branch switch, worktree creation, reset, rebase, merge, cherry-pick,
  stash, clean, Git topology change, or push was performed.

## 2. Kernel identity and historical evidence

| Property | Value |
|---|---|
| Distribution / platform | OpenWrt 24.10.4 / Malta BE64 |
| Linux version | 6.6.110 |
| ELF size | 11,013,776 bytes |
| SHA-256 | `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc` |
| ELF entry | `0xffffffff80917670` |
| Guest RAM / byte order | 256 MiB / big-endian |
| ROM/PROM | Existing accepted Malta ELF handoff |
| ROM base / size | `0x1fc00000` / 4,194,304 bytes |
| ROM content identity | `0xfa887be906ff0340` |

The kernel was neither patched nor regenerated. The Phase 29 Config0 trace
(382 records) remains SHA-256
`0b9248ad337e4aa5d0c434391b6c9ffb4d1ba3e8c28b26a0bdc0a5a339c2d9d2`; Phase 30
(4 records) remains `601d44baa44b0e17c46a0c9d8c88414d5fc9f7be4ef3cb55dd41083ba9527e68`.
The accepted Phase 31 four-record trace remains
`fbd854a39ae96864abfb20535a6b7e99bfc8d08047a7e29f7bfdea11cce6fcd4`; the
accepted Phase 32 four-record trace remains
`e55c6606bc9bbb2cd792ca6cc7fdc8f86e66c725b345ce8e6a3010bd9ab68f2e`.
All were re-hashed and left intact. Replay checks the accepted trace chain and
reconstructs the authenticated startup and continuation prefix; it does not
require an artificial 256-record match from the short Phase 31/32 traces.

## 3. Pre-implementation MOVN frontier

Before changing the CPU decoder, the production Malta VM replayed the
authenticated prefix, including DINSU and DINS, and captured the actual
pre-MOVN state. This capture is retained in the ignored build artifact
`build/mips64-12-artifacts/mips64-33-preimplementation-movn-capture.jsonl`.
The trace below independently records the same evidence.

The actual instruction bytes at virtual PC `0xffffffff8010b8f4` and physical
fetch address `0x00010b8f4` were `00 43 28 0b`. Big-endian assembly independently
produces `0x0043280b`:

| Field | Bits | Value |
|---|---:|---:|
| Primary opcode | 31:26 | SPECIAL `0x00` |
| `rs` | 25:21 | 2 (`$v0`) |
| `rt` | 20:16 | 3 (`$v1`) |
| `rd` | 15:11 | 5 (`$a1`) |
| Fixed-zero `sa` | 10:6 | 0 |
| Function | 5:0 | MOVN `0x0b` |

The mnemonic is `movn $5, $2, $3` (`MOVN $a1,$v0,$v1`). Before
implementation the decoder classified this word as valid-but-unsupported.
The live operands—not the value of `$v1` observed 27 instructions earlier at
DINS—were:

| Pre-instruction observation | Value |
|---|---:|
| `$v0` source | `0x0000000000000002` |
| `$v1` condition | `0x0000000000000000` |
| `$a1` destination | `0x0000000000000003` |
| Condition `GPR[rt] != 0` | False |
| Independent expected `$a1` | `0x0000000000000003` |
| PC / delay slot | `0xffffffff8010b8f4` / clear |
| CP0 Status / Cause / EPC / BadVAddr | `0x400080 / 0 / 0 / 0` |
| Count / VM cycles / retired | `158294 / 158294 / 158294` |

An authentic full-VM v3 snapshot was captured at this boundary before MOVN
implementation and then used for execution and replay. Its checksum is
`0xff60849fd5aa0264`, serialized size 268,437,646 bytes. The snapshot records
CPU v6 (1,001 bytes), memory v2, UART v4, and IRQ routing v1.

## 4. Architecture and implementation

The [MIPS64 Architecture for Programmers, Volume II, Revision 2.50](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf),
“MOVN — Move Conditional on Not Zero” (printed page 246), specifies a GPR
transfer when `GPR[rt]` is nonzero. The implemented behavior is:

```text
if GPR[rt] != 0:
    GPR[rd] = GPR[rs]
else:
    GPR[rd] is unchanged
```

`Mips64Op::Movn` is appended to preserve existing enum values. SPECIAL function
`0x0b` decodes as MOVN only when bits 10:6 are zero; each nonzero `sa` value is
classified as `ReservedInstruction`. Execution reads the original source and
condition as 64-bit values before writing. A true condition copies all 64
source bits through `writeGpr`; a false condition performs no destination
write. `$zero` therefore follows the existing helper semantics. The common
execution path handles PC, delay slots, Count, VM cycles, and retirement.
Disassembly uses `movn rd,rs,rt`, and the operation-name mapping returns
`movn`.

MOVZ (`SPECIAL` function `0x0a`) and floating-point conditional move
instructions remain distinct and unsupported. No floating-point or unrelated
conditional-move operation was added. MOVN adds no persistent state and
requires no snapshot format change.

## 5. Encoded MOVN qualification and replay

The new `test_mips64_movn` / `Mips64MovnTests` target executes encoded
instructions and reports **1,285 passed, 0 failed**. It checks the exact Linux
word, field extraction, fixed-zero encoding, reserved shifts, disassembly,
operation naming, and classification of MOVZ and FP MOVN. Data cases include
conditions 1, 2, all ones, bit 63 only, bit 32 only, and zero; full-width
positive, negative, minimum-signed, all-one, high-word, and low-word-zero
sources; and exact false-path preservation including destination bit 63.

The suite covers `rd==rs`, `rd==rt`, `rs==rt`, all fields identical, and `$zero`
in each role, with true and false alias cases. It runs both endian modes and
checks PC, Count, GPRs, CP0, HI/LO, TLB, memory, interrupt lines, reservations,
and no-exception retirement. It also executes SLTI, SLTIU, SLT, and SLTU
regressions; taken and untaken branch delay slots; CPU serialization; and
synthetic production VM snapshots with true, false, high-word, and alias
conditions. Synthetic full-VM replay includes an armed delay slot and live LLD
reservation in both endian modes. A checksum-corrupted snapshot is rejected
transactionally without advancing time or changing CPU state.

All VM snapshot formats remain `GXM64SNP` v3, CPU v6 (1,001 bytes), memory v2,
UART v4, and IRQ routing v1. Replay comparison covers the full snapshot
integrity checksum; it normalizes only the console's monotonic host-side
`generation` field, while guest-visible console bytes and sequence remain
compared.

## 6. Production Malta execution and authentic continuation

The production runner reconstructed the accepted Phase 29–32 path, verified
the unchanged kernel and ROM identities, and reached the actual MOVN word.
The independent oracle selected the false path because the live `$v1` was
exactly zero. Production `VirtualMachine::run(1)` retired MOVN:

| Observation | Before | After MOVN |
|---|---:|---:|
| `$a1` | `0x0000000000000003` | `0x0000000000000003` |
| PC | `0xffffffff8010b8f4` | `0xffffffff8010b8f8` |
| Count / VM cycles / retired | `158294 / 158294 / 158294` | `158295 / 158295 / 158295` |
| Delay slot | Clear | Clear |
| Cause / EPC | `0 / 0` | `0 / 0` |

All other GPRs were unchanged. CP0 state changed only by ordinary Count
progression. HI/LO, TLB, reservation state, and memory did not change. MOVN
raised no architectural exception. Restoring the pre-MOVN full-VM snapshot
reproduced the same CPU result and full snapshot state.

After MOVN, the unchanged kernel retired **178 additional instructions** and
then reached a valid MFC0 instruction whose CP0 register selector is not
implemented by this CPU subset:

| Frontier field | Value |
|---|---:|
| PC / fetch PA | `0xffffffff8010c1e4` / `0x00010c1e4` |
| Bytes / word | `40 02 60 02` / `0x40026002` |
| Opcode / `rs` / `rt` / `rd` / `sel` | `COP0 0x10 / 0 / 2 / 12 / 2` |
| Operation | `mfc0 $2, $12, 2` |
| CPU stop | `Cp0Violation`: `MFC0 to unsupported CP0 register/selector` |
| Architectural exception entry | No |

The full VM was saved before the failed MFC0 attempt. The first production
attempt advanced CP0 Count from 158,473 to 158,474, but VM cycles and retired
instructions both remained 158,473; PC remained at the faulting MFC0 and the
VM entered `ERROR`. Restoring the valid pre-failure snapshot into a fresh
production Malta session and attempting the same MFC0 reproduced the same CPU
fault and serialized state. The failed MFC0 is not counted as retired.

| Continuation accounting | Value |
|---|---:|
| Total retired | 158,473 |
| Retired since Linux entry | 158,456 |
| Retirements beyond the former MOVN frontier | 179 (MOVN plus 178 more) |
| Retired after MOVN | 178 |
| VM attempts after MOVN, including the failed MFC0 | 179 |
| Canonical total attempts, including one failed MFC0 | 158,474 |
| Pre-failure Count / cycles / retirement | `158473 / 158473 / 158473` |
| Post-failure Count / cycles / retirement | `158474 / 158473 / 158473` |

Authentic replay compares CPU/GPR and CP0 state, RAM, reservations, devices,
UART/console, scheduler state, VM cycles, retirement, and stop location. The
next frontier is a runtime CP0 access fault rather than a decoder-level
unsupported instruction. The deterministic continuation budget was one
million attempts and was not exhausted.

## 7. Device observations and trace integrity

At the frontier, Status was `0x400080`; Cause, EPC, and BadVAddr were zero;
the delay slot was clear; there were no valid TLB entries, no active LL/SC
reservation, and no pending interrupt-controller event. UART TX, RX FIFO, and
console output were all zero bytes. MMIO was not instrumented, so no MMIO
activity count is claimed. Full Linux boot remains unverified.

The four-record JSONL trace is
[`docs/artifacts/openwrt_malta_be64_movn_phase33.jsonl`](artifacts/openwrt_malta_be64_movn_phase33.jsonl),
SHA-256
`d4d37456d4836aafcd31e4efab7d7d64bda05f6be46e132e0d5a7cdd7e0f1f28`.
Every line parsed as JSON. It records the preimplementation-equivalent live
operands, instruction decode, pre-instruction snapshot identity and versions,
MOVN retirement, continuation counts, valid pre-failure frontier, failed
attempt accounting, device observations, and deterministic replay result.

## 8. Regression and production linkage

- Final MIPS64 CTest: **61/61 passed** — all 60 inherited suites plus
  `Mips64MovnTests`.
- Selected cross-ISA CTest: **6/6 passed** — `BasicTests`,
  `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`,
  `VMCheckpointTests`, and `RiscVDecoderSmokeTest`.
- All registered MIPS64 test executables and `build/bin/ia64emu.exe` built.
  Production `nm -C` output contains `mips64::decodeInstruction`,
  `Mips64Cpu::executeDecoded`, `Mips64Cpu::step`, `Mips64Cpu::disassemble`,
  and `Mips64Cpu::opName`; the executable's strings include `movn`. The
  production runner separately verified actual decode, execution, and result.
- `ninja -C build all -j6` did not complete because `debug_harness.exe` fails
  to link. Re-running `ninja -C build debug_harness -j2` reproduced unresolved
  MinGW imports from `RestAPIServer.cpp` and `WebUIServer.cpp`: `__imp_WSAStartup`,
  `__imp_socket`, `__imp_WSAGetLastError`, `__imp_WSACleanup`, `__imp_htons`,
  `__imp_bind`, `__imp_listen`, `__imp_closesocket`, `__imp_accept`,
  `__imp_recv`, `__imp_send`, and `__imp_setsockopt`. No unrelated Winsock or
  debug-harness source was changed. The MIPS64 production target and all
  required MIPS64 tests built independently.

The two DEXT neighbor tests also contained stale expectations that DINS was
unsupported after Phase 32. Their assertions now verify the already accepted
DINS decode and disassembly, allowing the rebuilt inherited regression suite
to protect DINS rather than reject it.

## 9. Limits and MIPS64-34 recommendation

This phase establishes MOVN and the next authentic stop only. It does not
claim Linux boot completion or uninstrumented MMIO results. MIPS64-34 should
inspect the guest's `MFC0` access to CP0 register 12, selector 2 against the
architecture and the surrounding kernel code, implement only the required
selector semantics with focused CP0/snapshot tests, then replay the same
unchanged Malta kernel to its next frontier. Avoid speculative CP0 or unrelated
ISA expansion.
