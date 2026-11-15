# MIPS64-31 — DINSU and authentic OpenWrt continuation

**Outcome A — complete.** The unchanged OpenWrt Malta BE64 kernel reached its
authenticated DINSU instruction, the production VirtualMachine retired it with
the independently calculated partial-register result, and Linux stopped at the
next valid-but-unsupported instruction. Full Linux boot is not claimed.

## 1. Repository preflight

- Checkout: `D:\dev\guideXOS_Hypervisor_MIPS64`; branch: `MIPS64`.
- Starting HEAD: `1b55ae8697c5718865595b9f0fd54da83608e1b1`, the accepted
  MIPS64-30 commit. The branch matched `origin/MIPS64` at preflight (0 ahead,
  0 behind).
- The initial tracked and untracked worktree was clean. The only ignored
  top-level paths were the existing `build/` directory and
  `tests/fixtures/mips64_elf/.objects/`; the kernel was reused in place and
  its hash was checked before and after execution.
- `.phase-mips64` recorded MIPS64-30 complete and MIPS64-31 next. The separate
  IA-64 `.phase` file and IA-64 source were not changed.
- The Phase 20 DEXT, Phase 25 DEXTM, Phase 29 Config0, and Phase 30 EXT reports
  and the accepted Phase 30 trace were read. The SPECIAL3 decoder, DEXTM/EXT
  field handling, register access, retirement, disassembly, CPU serialization,
  and production full-VM snapshot replay paths were inspected.
- `E:\CodexArtifacts\guideXOS_Hypervisor` was inspected; a targeted filename
  search found no MIPS64/OpenWrt/Malta kernel artifacts there. The documented
  kernel at `build/mips64-12-artifacts/openwrt-24.10.4-malta-be64-vmlinux.elf`
  was reused; no guest image or historical trace was regenerated.
- Before DINSU implementation, all 58 inherited MIPS64 CTest suites passed.

No branch, worktree, or Git topology operation was performed. Nothing was
pushed.

## 2. Kernel identity and accepted evidence

| Property | Value |
|---|---|
| Distribution / platform | OpenWrt 24.10.4 / Malta BE64 |
| Linux version | 6.6.110 |
| Guest ELF size | 11,013,776 bytes |
| SHA-256 | `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc` |
| Entry | `0xffffffff80917670` |
| RAM / byte order | 256 MiB / big-endian |
| PROM command line | `console=ttyS0,38400n8` |
| Malta ROM/PROM | Existing accepted configuration |

The guest was not patched or regenerated. The accepted Phase 29 trace remains
SHA-256
`0b9248ad337e4aa5d0c434391b6c9ffb4d1ba3e8c28b26a0bdc0a5a339c2d9d2`.
The accepted four-record Phase 30 trace remains SHA-256
`601d44baa44b0e17c46a0c9d8c88414d5fc9f7be4ef3cb55dd41083ba9527e68`.
The pre-implementation production replay regenerated those same four records
byte-for-byte and produced that same full hash.

The Phase 30 baseline was 158,265 total retired instructions, including 50
after EXT, and ended at PC `0xffffffff8010ba74` with word `0x7ca3f806`. The
existing production EXT harness reproduced that boundary before DINSU changes.

## 3. Authenticated DINSU encoding and prestate

The production VM fetched the actual guest bytes `7c a3 f8 06` at virtual PC
`0xffffffff8010ba74` and physical address `0x00010ba74`. Big-endian byte
assembly independently yields `0x7ca3f806`.

| Field | Bits | Value |
|---|---:|---:|
| Primary opcode — SPECIAL3 | 31:26 | `0x1f` |
| `rs` — `$a1`, source | 25:21 | 5 |
| `rt` — `$v1`, destination | 20:16 | 3 |
| `msbminus32` | 15:11 | 31 |
| `lsbminus32` | 10:6 | 0 |
| Function — DINSU | 5:0 | `0x06` |

Thus `pos = lsbminus32 + 32 = 32`, `msb = msbminus32 + 32 = 63`, and
`size = msb - pos + 1 = 32`. Disassembly is
`dinsu $3, $5, 32, 32`. Bits 15:11 encode the upper insertion bound; they are
not a destination register. The destination is `rt`.

The actual pre-instruction operands, captured before implementing DINSU, were:

| Observation | Value |
|---|---:|
| `$a1` source | `0x0000000000000010` |
| `$v1` destination before | `0x0000000000000000` |
| `$v1` low 32 bits before | `0x00000000` |
| Position / size | 32 / 32 |
| Field mask | `0xffffffff00000000` |
| Preserved-bit mask | `0x00000000ffffffff` |
| Status / Cause / EPC / ErrorEPC / BadVAddr | `0x400080 / 0 / 0 / 0 / 0` |
| Count / VM cycles / retired | 158,265 / 158,265 / 158,265 |
| Delay slot armed | No |

The expected result was calculated independently from the encoding and
prestate: `source_low32 = 0x10`, and
`(source_low32 << 32) | (destination_before & 0xffffffff)` equals
`0x0000001000000000`. The original low word is preserved. This actual
destination happened to have a zero low word; synthetic cases separately use
nonzero low words to verify preservation.

## 4. Implementation and field policy

`Mips64Op::Dinsu` is appended to the enum so prior operation values remain
stable. SPECIAL3 function `0x06` decodes `rs` and `rt` as source and destination
and retains the encoded upper fields in `rd` and `sa`. Decode rejects
`lsbminus32 > msbminus32` as `UnsupportedInstruction` with an explicit
UNPREDICTABLE DINSU diagnostic. It does not enter an architectural exception.

Execution checks the encoded relationship and legal range before subtraction
or shifting. For valid fields, it reads both source and original destination
before any write, forms a `uint64_t` low mask, positions the source field, and
merges it with `originalDestination & ~fieldMask`. It writes the full 64-bit
result through `writeGpr(rt)`, preserving all unselected bits and `$zero`
semantics. It does not sign-extend or truncate the result. The existing common
retirement path advances PC, Count, and VM cycles.

DINSU has `32 <= pos < 64`, `1 <= size <= 32`, and `pos + size <= 64`; with its
five-bit fields, `rd >= sa` describes every defined field. The reversed field
case is stopped before unsigned underflow or mutation. DINS function `0x07` and
DINSM function `0x05` remain valid-but-unsupported and retain distinct
diagnostics. Other allocated SPECIAL3 instructions remain unsupported, and
unallocated function `0x08` remains reserved. DINS and DINSM are not
implemented in this phase.

The implementation follows the [MIPS64 Volume II-A, Revision 5.04 DINSU
definition (printed page 149)](https://s3-eu-west-1.amazonaws.com/downloads-mips/documents/MD00087-2B-MIPS64BIS-AFP-05.04.pdf),
with operand-field cross-checks from [GNU Binutils' MIPS opcode
definitions](https://gnu.googlesource.com/binutils-gdb/+/refs/heads/master/include/opcode/mips.h)
and [Android's MIPS64 assembler DINSU
encoder](https://android.googlesource.com/platform/art/+/3a29355967f29999ba81b5772ec66d3ddee9ce3e/compiler/utils/mips64/assembler_mips64.cc).

## 5. Encoded architectural qualification

The new `test_mips64_dinsu` / `Mips64DinsuTests` target executes actual encoded
instructions. It reports **1,066 passed, 0 failed**. Coverage includes:

- Exact Linux word, SPECIAL3/function/register fields, encoded upper fields,
  position/size formulas, operation name, and disassembly.
- Positions 32, 33, 40, 47, and 63; widths 1, 8, 9, and 32; fields ending at
  bit 63 or before bit 63; minimum and maximum widths.
- Zero, all-one, alternating, bit-zero-only, bit-31-only, and high-word source
  data; zero, all-one, and mixed destination data; selected-field replacement
  and preservation outside partial fields.
- Source/destination aliasing, source `$zero`, destination `$zero`, both
  endiannesses, normal PC/Count retirement, and no unrelated GPR, CP0, HI/LO,
  memory, or reservation changes.
- Taken and not-taken branch delay slots; CPU serialization before DINSU and
  replay; full-VM v3 snapshot replay with a live LLD reservation and DINSU in
  an armed delay slot.
- Snapshot replays for minimum/maximum-width and bit-63 insertions, plus
  source/destination aliasing. Corrupt snapshot restore is rejected without
  changing live state or advancing VM cycles.
- UNPREDICTABLE field ordering, valid-but-unsupported DINS and DINSM, reserved
  SPECIAL3 classification, and neighboring EXT/DEXT/DEXTM decoding.

DINSU adds no persistent state. Formats remain full VM `GXM64SNP` v3, CPU v6
(1,001 bytes), memory v2, UART v4, and IRQ routing v1.

## 6. Production Malta retirement and replay

The production VirtualMachine replay matched all 256 Phase 29 startup records,
the Config0 boundary, eight following accepted instructions, and the accepted
EXT result before reaching DINSU. A full-VM snapshot was captured immediately
before the actual DINSU. The instruction was retired, the snapshot restored,
and DINSU plus the bounded continuation replayed. CPU/GPR/CP0 state, RAM and
reservation generations, device state, UART/console, scheduler state, VM cycles,
and final stop reason matched.

| Observation | Before | After DINSU |
|---|---:|---:|
| `$v1` | `0x0000000000000000` | `0x0000001000000000` |
| `$a1` | `0x0000000000000010` | unchanged |
| PC | `0xffffffff8010ba74` | `0xffffffff8010ba78` |
| Count / VM cycles / retired | 158,265 / 158,265 / 158,265 | 158,266 / 158,266 / 158,266 |
| Delay slot | Clear | Clear |
| Cause / EPC | 0 / 0 | 0 / 0 |

All other GPRs, non-Count CP0 registers, HI/LO, reservation state, and guest
memory remained unchanged at retirement. The original low 32 destination bits
remained zero; the high 32 became the low 32 bits of `$a1`. Snapshot replay
reproduced the same result and continuation.

## 7. Next frontier and bounded stop

The unchanged guest retired one instruction beyond the former DINSU frontier
and immediately reached the valid-but-unsupported DINS word `0x7ca3f807` at
PC `0xffffffff8010ba78` (physical `0x00010ba78`). Its fields independently
decode as SPECIAL3, `rs=5`, `rt=3`, `rd=31`, `sa=0`, function `0x07`, or
`DINS $v1,$a1,0,32`. The instruction was not executed. No architectural
exception was entered.

| Final observation | Value |
|---|---:|
| Total retired / attempted | 158,266 / 158,267 |
| Retired since Linux entry | 158,249 |
| Retirements beyond former DINSU frontier | 1 (DINSU) |
| Retired after DINSU | 0 |
| Stop reason | `unsupported_instruction` at DINS |
| Count / VM cycles | 158,266 / 158,266 |
| Status / Cause / EPC / ErrorEPC / BadVAddr | `0x400080 / 0 / 0 / 0 / 0` |
| Delay slot / valid TLB entries | Clear / 0 |
| IRQ lines 0–5 | All inactive |
| UART TX / RX / console output | 0 / 0 / 0 bytes |
| MMIO | Not instrumented; no count claimed |

The deterministic continuation budget was one million attempts and was not
exhausted. Linux boot completion remains unverified.

## 8. Trace integrity, regressions, and production linkage

The distinct trace
[`docs/artifacts/openwrt_malta_be64_dinsu_phase31.jsonl`](artifacts/openwrt_malta_be64_dinsu_phase31.jsonl)
contains four JSONL records and has SHA-256
`fbd854a39ae96864abfb20535a6b7e99bfc8d08047a7e29f7bfdea11cce6fcd4`. It
records the authenticated word and operands, independent expected result,
retirement, replay summary, device/interrupt observations, and the next DINS
frontier. Phase 29 and Phase 30 trace hashes and the kernel hash remain
unchanged.

- Pre-implementation MIPS64 baseline: **58/58 CTest suites passed**.
- Final MIPS64 CTest: **59/59 suites passed** (58 inherited plus DINSU).
- DINSU-focused target: **1,066/1,066 passed**.
- Selected IA-64/RISC-V regressions: **6/6 passed** — `BasicTests`,
  `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`,
  `VMCheckpointTests`, and `RiscVDecoderSmokeTest`.
- All affected MIPS64 targets and production `ia64emu.exe` built. `nm -C`
  shows production-linked `decodeInstruction`, `Mips64Cpu::executeDecoded`,
  `step`, `disassemble`, and `opName`; the executable contains the DINSU
  mnemonic and UNPREDICTABLE diagnostics.
- The aggregate all-target build also reaches an existing unrelated
  `debug_harness.exe` link failure for unresolved Windows Winsock symbols under
  this MinGW setup. The affected MIPS64 targets and `ia64emu.exe` were built
  directly, and all required test suites passed; no unrelated source was
  changed.

## 9. Limits and MIPS64-32 recommendation

This phase qualifies only DINSU and the exact next guest frontier. The next
phase should authenticate and qualify the observed DINS word
`0x7ca3f807` at `0xffffffff8010ba78`, then continue the same unchanged kernel
with bounded replay. Keep DINSM and other SPECIAL3 operations unsupported
unless the guest reaches them and their architectural behavior is separately
qualified. Do not claim Linux boot completion or uninstrumented MMIO activity.
