# MIPS64-32 — DINS and authentic OpenWrt continuation

**Outcome A — complete for the authenticated DINS instruction and the next
Linux frontier.** The production Malta VM retired DINS with the independently
calculated full-register result, replayed the authentic DINSU/DINS pair and the
bounded continuation deterministically, and reached the next valid but
unsupported instruction. Full Linux boot is not claimed.

## 1. Repository preflight

- Repository: `D:\dev\guideXOS_Hypervisor_MIPS64`; branch: `MIPS64`.
- Starting HEAD: `0926f27f4e0cd2e674f27d4867f24ffa67f11d94`, the accepted
  MIPS64-31 commit. At preflight the branch was one commit ahead and zero
  behind `origin/MIPS64`. The local branch and checkout were treated as the
  authority.
- Tracked and untracked worktree state was clean before edits. Existing ignored
  paths were `build/` and `tests/fixtures/mips64_elf/.objects/`.
- `.phase-mips64` recorded MIPS64-31 complete and MIPS64-32 next. The separate
  IA-64 `.phase` marker and IA-64 sources were not changed.
- The MIPS64-30 EXT and MIPS64-31 DINSU reports, SPECIAL3 decoder, DINSU
  execution and tests, DEXT/DEXTM and EXT regressions, and CPU/full-VM snapshot
  paths were inspected.
- `E:\CodexArtifacts\guideXOS_Hypervisor` was inspected. It contained no
  MIPS64/OpenWrt/Malta artifact superseding the repository kernel and traces.
  No branch, worktree, or Git topology operation was performed, and nothing
  was pushed.
- Before implementation, the 59 inherited MIPS64 CTest suites passed 59/59.
  A broader 71-test CTest invocation also passed all 59 MIPS64 suites, while
  six unrelated non-MIPS tests were not run because their executables were
  absent from the existing build tree.

## 2. Kernel identity and preserved evidence

The unchanged guest is OpenWrt 24.10.4 for Malta BE64, Linux 6.6.110. Its ELF
is 11,013,776 bytes, SHA-256
`1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc`, and
entry `0xffffffff80917670`. The production VM used 256 MiB of big-endian RAM,
the accepted Malta ROM/PROM ELF handoff, and command line
`console=ttyS0,38400n8`. The kernel was not regenerated, patched, or committed.

Historical trace hashes remain:

| Evidence | Records | SHA-256 |
|---|---:|---|
| Phase 29 Config0 trace | 382 | `0b9248ad337e4aa5d0c434391b6c9ffb4d1ba3e8c28b26a0bdc0a5a339c2d9d2` |
| Phase 30 EXT trace | 4 | `601d44baa44b0e17c46a0c9d8c88414d5fc9f7be4ef3cb55dd41083ba9527e68` |
| Phase 31 DINSU trace | 4 | `fbd854a39ae96864abfb20535a6b7e99bfc8d08047a7e29f7bfdea11cce6fcd4` |

All three accepted traces were re-hashed before replay. The new bounded trace
is [`docs/artifacts/openwrt_malta_be64_dins_phase32.jsonl`](artifacts/openwrt_malta_be64_dins_phase32.jsonl): four JSONL records, 2,794 bytes,
SHA-256 `e55c6606bc9bbb2cd792ca6cc7fdc8f86e66c725b345ce8e6a3010bd9ab68f2e`.

## 3. Reproduced DINS frontier and independent decode

Before changing the CPU implementation, the production Malta VM replayed the
accepted Linux prefix, retired the accepted DINSU at
`0xffffffff8010ba74`, and stopped at the actual DINS frontier. The accepted
Phase 31 summary did not contain the DINS PC or total retirement count; those
values below came from the replay.

| Observation before DINS | Value |
|---|---:|
| Virtual PC / fetch PA | `0xffffffff8010ba78` / `0x00010ba78` |
| Fetched bytes / reconstructed BE word | `7c a3 f8 07` / `0x7ca3f807` |
| `$a1` source | `0x0000000000000010` |
| `$v1` destination | `0x0000001000000000` |
| CP0 Status / Cause / EPC | `0x400080` / `0` / `0` |
| Count / VM cycles / retired | `158266 / 158266 / 158266` |
| Branch delay armed | No |

The word fields are SPECIAL3 opcode `0x1f`, `rs=5` (`$a1`), `rt=3` (`$v1`),
`rd=31` (`msb`), `sa=0` (`lsb`), and function `0x07`. Thus the operation is
`DINS $v1,$a1,0,32`; `rt`, not `rd`, names the destination.

The encoding was checked separately with the LLVM 18.1.8 MIPS64r2 integrated
assembler. Assembling `dins $3, $5, 0, 32` produced the bytes `7c a3 f8 07`.
The [MIPS64 Volume II, Revision 2.50 DINS definition, printed pages 140–141](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf)
defines `rd` as `msb`, `sa` as `lsb`, and the merge of the selected source bits
into the existing destination. [GNU Binutils' MIPS operand definitions](https://gnu.googlesource.com/binutils-gdb/+/refs/heads/master/include/opcode/mips.h)
independently describes the DINS position and size fields.

For the live operands, the insertion mask is `0x00000000ffffffff` and the
preserved mask is `0xffffffff00000000`. A separate bit-by-bit oracle calculated
the result as `0x0000001000000010`. This was captured before implementation;
the value was not inferred from the later execution.

## 4. DINS semantics and field policy

For valid DINS, `pos = lsb`, `size = msb - lsb + 1`, with
`0 <= pos < 32`, `1 <= size <= 32`, and `pos + size <= 32`. It inserts the
rightmost `size` bits of `rs` into the selected field of the complete 64-bit
`rt` value, preserving every other destination bit. For the authenticated
word:

```text
new_v1 = (old_v1 & 0xFFFFFFFF00000000) | (old_a1 & 0xFFFFFFFF)
       = 0x0000001000000010
```

DINS does not truncate or sign-extend the destination. Its source is a full
64-bit GPR; no sign-extended-word precondition is imposed. DINSU remains the
upper-word insertion operation, and DINSM remains a separate valid but
unsupported SPECIAL3 instruction. Function `0x05` is not decoded as DINS.

The architecture calls `lsb > msb` UNPREDICTABLE. Decode and execution validate
the field order before subtraction or shifts. The emulator's deterministic
policy is a typed `UnsupportedInstruction` stop without guest exception entry
or destination mutation. Tests exercise both fetched and manually constructed
invalid decoded instructions.

## 5. Implementation and encoded tests

`Mips64Op::Dins` is appended to preserve existing enum values. SPECIAL3 function
`0x07` decodes `rs`, `rt`, `rd=msb`, and `sa=lsb`; execution reads source and
original destination before writing a 64-bit field merge. Disassembly and
operation naming emit `dins rt,rs,pos,size`. The separate DINSU path is
unchanged.

The new `test_mips64_dins` / `Mips64DinsTests` target executes encoded
instructions and reports **1,049 passed, 0 failed**. Coverage includes the
authenticated word, decode fields, exact disassembly, both endiannesses,
positions 0, 1, 8, 15, 16, and 31, widths 1, 8, 16, and 32, exact and partial
bit-31 boundaries, source/destination patterns, 64-bit non-sign-extended
sources, ignored source high bits, aliasing, both `$zero` roles, and
UNPREDICTABLE rejection. It also checks unrelated GPRs, CP0, HI/LO, TLB,
reservations, and guest memory; CPU serialization; and taken and untaken branch
delay slots.

Synthetic production-VM tests snapshot and replay DINS at full-word, partial,
and bit-31 boundaries in both endiannesses, including aliasing, an armed branch
delay slot, and a live LLD reservation. They compare CPU, memory, devices,
scheduler and cycle state. A malformed snapshot is rejected without changing
live VM state or advancing time.

No snapshot version changed: full VM `GXM64SNP` v3, CPU v6 (1,001 bytes),
memory v2, UART v4, and IRQ routing v1. DINS adds no persistent state. The
neighboring DINSU target now reports **1,068 passed, 0 failed**, including the
new supported-DINS classification check while continuing to distinguish
DINSM.

## 6. Authentic production retirement and replay

The unchanged OpenWrt kernel reached the exact DINS instruction in the
production Malta VM. An in-memory full-VM checkpoint was captured immediately
before DINS. The operation retired normally:

| Observation | Before | After DINS |
|---|---:|---:|
| `$a1` | `0x0000000000000010` | unchanged |
| `$v1` | `0x0000001000000000` | `0x0000001000000010` |
| PC | `0xffffffff8010ba78` | `0xffffffff8010ba7c` |
| Count / VM cycles / retired | `158266 / 158266 / 158266` | `158267 / 158267 / 158267` |
| Delay slot | Clear | Clear |
| Cause / EPC | `0 / 0` | `0 / 0` |

The upper 32 destination bits stayed `0x00000010`; the selected lower 32 bits
became `0x00000010`. Every other GPR, non-Count CP0 state, HI/LO, TLB,
reservation generation, and guest memory remained unchanged. The valid
instruction raised no architectural exception.

Full-VM replay was checked from immediately before DINS and immediately before
the authentic DINSU/DINS pair. The replay reproduced DINSU's accepted
`$v1=0x0000001000000000` state, the pre-DINS checkpoint, and DINS's final
state. The continuation was restored from the pre-DINS checkpoint and rerun;
CPU, RAM, reservation generations, devices, UART/console, scheduler, cycles,
instruction trace window, and final stop reason matched.

## 7. Linux continuation and next frontier

Linux retired **158,294** instructions total, **158,277** since ELF entry. It
retired 27 instructions after DINS (28 instructions beyond the former DINS
frontier, counting DINS). The bounded window used 28 additional attempts,
including the stopping attempt, within the one-million-attempt budget.

The next genuine frontier is virtual PC `0xffffffff8010b8f4`, fetch PA
`0x00010b8f4`, bytes `00 43 28 0b`, word `0x0043280b`. It decodes as SPECIAL
function `0x0b`, `rs=2`, `rt=3`, `rd=5`, or `MOVN $a1,$v0,$v1`. LLVM's
MIPS64r2 assembler/disassembler independently confirmed this encoding and
mnemonic. MOVN remains unsupported in this phase.

At the bounded stop, Count, VM cycles, and total retirement were 158,294;
Status was `0x400080`, Cause and EPC were zero, no branch delay was armed, no
TLB entries were valid, and IRQ lines 0–5 were inactive. UART TX, RX FIFO, and
console output were all zero bytes. MMIO was not instrumented, so no MMIO
activity count is claimed. The guest did not complete a full Linux boot.

## 8. Regression and production build results

- Final MIPS64 CTest: **60/60 passed** (59 inherited suites plus
  `Mips64DinsTests`). This includes EXT, DEXT/DEXTM, DINSU, CP0, exception,
  branch, TLB/MMU, LL/SC, Malta, UART, and snapshot regressions.
- Selected cross-ISA regressions: **6/6 passed** — `BasicTests`,
  `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`,
  and `RiscVDecoderSmokeTest`.
- `test_mips64_dins`, `test_mips64_dinsu`, and production `ia64emu.exe` built.
  `nm -C` confirms production-linked MIPS64 `decodeInstruction`,
  `Mips64Cpu::executeDecoded`, `step`, `disassemble`, and `opName`; the
  executable contains the DINS mnemonic and UNPREDICTABLE diagnostics.
- The all-target aggregate build was not completed; this existing build tree
  began a 766-step build and was stopped at 43 steps to isolate the known
  unrelated target. Building `debug_harness` directly reproduced the MinGW
  Winsock link failure: unresolved `__imp_WSAStartup`, `__imp_socket`,
  `__imp_bind`, `__imp_listen`, `__imp_accept`, `__imp_send`, `__imp_recv`,
  `__imp_closesocket`, and related imports from `RestAPIServer.cpp` and
  `WebUIServer.cpp`. No unrelated networking or debug-harness code was
  changed. Required MIPS64 targets and production `ia64emu.exe` built
  successfully.

## 9. Limits and MIPS64-33 recommendation

This phase qualifies DINS and the exact Linux continuation only. It does not
claim full Linux boot or uninstrumented MMIO observations. The next phase should
authenticate and qualify the observed `MOVN $a1,$v0,$v1` at
`0xffffffff8010b8f4`, recording its live condition and destination values,
then continue the same unchanged kernel. Keep other unsupported SPECIAL
operations out of scope unless reached by the guest.
