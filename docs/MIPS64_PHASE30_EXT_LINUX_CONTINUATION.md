# MIPS64-30 — EXT and authentic OpenWrt continuation

**Outcome: A — complete for the authenticated EXT instruction and the next
Linux frontier.** The unchanged OpenWrt Malta BE64 kernel fetched and retired
the authenticated SPECIAL3 function `0x00` instruction. It then retired 50
more instructions and stopped at a different unsupported SPECIAL3 function
`0x06` instruction. Linux boot completion is not claimed.

## 1. Repository preflight

- Checkout: `D:\dev\guideXOS_Hypervisor_MIPS64`; branch: `MIPS64`.
- Starting HEAD: `6ff06b86847ea0e45b29ad07cfdc22c1d4e786f4`, the required
  MIPS64-30 base. The worktree was clean; upstream divergence was two commits
  ahead and zero behind. No branch or Git topology changes were made.
- `.phase-mips64` recorded MIPS64-29 complete and MIPS64-30 next. The IA-64
  `.phase` marker and IA-64 sources were left untouched.
- The MIPS64-20 DEXT, MIPS64-25 DEXTM, and MIPS64-29 Config0 reports and the
  Phase 29 accepted trace were inspected. SPECIAL3 decoding, DEXT/DEXTM
  execution, bit-field test helpers, signed-word helpers, production VM
  snapshots, and the prior disassembly/reporting path were reviewed.
- `E:\CodexArtifacts\guideXOS_Hypervisor` and documented MIPS64 artifact
  locations were checked; no separate MIPS64 phase artifact superseded the
  repository kernel and trace.
- Before editing, all 57 inherited MIPS64 CTest suites passed.

## 2. Kernel and historical evidence

The unchanged OpenWrt 24.10.4 Malta BE64 / Linux 6.6.110 ELF is
`build/mips64-12-artifacts/openwrt-24.10.4-malta-be64-vmlinux.elf`, 11,013,776
bytes, entry `0xffffffff80917670`, SHA-256
`1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc`. The
accepted 256 MiB RAM and ROM/PROM handoff were reused without guest changes.

The preserved Phase 29 trace,
`docs/artifacts/openwrt_malta_be64_config0_phase29.jsonl`, remains 382 records
with SHA-256
`0b9248ad337e4aa5d0c434391b6c9ffb4d1ba3e8c28b26a0bdc0a5a339c2d9d2`.
Neither it nor the kernel was replaced. The new bounded trace is
`docs/artifacts/openwrt_malta_be64_ext_phase30.jsonl`, four JSON records, SHA-256
`601d44baa44b0e17c46a0c9d8c88414d5fc9f7be4ef3cb55dd41083ba9527e68`.

## 3. Reconciliation of the Phase 29 mnemonic

Before implementation, the accepted Phase 29 continuation was reproduced to
PC `0xffffffff8010b85c` using the production Malta VM. The VM fetched
`0x7cc30b40`; the live guest RAM bytes were `7c c3 0b 40`. The ELF load image
contains the same bytes at the corresponding physical offset. As BE instruction
bytes they independently assemble to `0x7cc30b40`.

| Field | Instruction bits | Value |
|---|---:|---:|
| SPECIAL3 opcode | 31:26 | `0x1f` |
| `rs` | 25:21 | 6 (`$a2`) |
| `rt` | 20:16 | 3 (`$v1`) |
| `msbd` | 15:11 | 1 |
| `lsb` | 10:6 | 13 |
| function | 5:0 | `0x00` |

For EXT, `size = msbd + 1` and `pos = lsb`, so the instruction is
**`EXT $v1,$a2,13,2`**. The MIPS64 Volume II Release 2.50 EXT definition gives
these position/size fields and the valid word-value condition; GNU Binutils'
MIPS opcode format independently assigns the position and size-minus-one
operands to the corresponding encoded fields. [MIPS64 Volume II, EXT, pp. 181–182](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf),
[GNU Binutils MIPS opcode definitions](https://gnu.googlesource.com/binutils-gdb/+/refs/heads/master/include/opcode/mips.h).
LLVM 18.1.8 configured for MIPS64r2 disassembled the word as
`ext $3, $6, 0xd, 0x2`.

The Phase 29 report's `EXT $v1,$a2,1,14` was a reporting transcription error,
not a different guest word or a production decoder result. The old test reporter
had a manually written `independent_decode` string with position and size
swapped. The wrong interpretation would encode as `0x7cc36840`; the fetched
word remains `0x7cc30b40`. Phase 30 corrects the reporter literal and documents
the correction here. The historical Phase 29 report and trace were preserved.

## 4. Implementation and architectural policy

The implementation adds `Mips64Op::Ext` at the end of the enum to retain
existing numeric values. SPECIAL3 function `0x00` decodes `rd` as `msbd`, `sa`
as `lsb`, `rt` as the destination, and `rs` as the source. It computes
`size = msbd + 1` and rejects `pos + size > 32` as a typed
`UnsupportedInstruction` with an explicit UNPREDICTABLE diagnostic. Execution
also validates the encoded range and checks
`source64 == signExtend32(low32(source64))` before writing any result. The
deterministic emulator policy for either UNPREDICTABLE condition is an
unsupported stop without guest exception entry or destination/memory mutation.
The attempt follows the existing Count accounting convention and does not
claim normal retirement.

For defined fields, extraction uses only the low 32 bits, right-justifies the
selected bits, clears unselected bits, handles size 32 without a shift by 32,
and sign-extends the resulting word to 64 bits. Source is read before the
destination write, `$zero` remains immutable, and normal retirement advances
PC/Count/cycles. Disassembly and operation naming now emit `ext` with
`pos,size`. DEXT, DEXTM, and DEXTU classifications remain distinct.

## 5. Encoded instruction and CPU coverage

`tests/test_mips64_ext.cpp` registers the `test_mips64_ext` executable and
`Mips64ExtTests` CTest suite. It uses encoded SPECIAL3 instructions and
independently computes expected fields and results. Coverage includes:

- Exact word `0x7cc30b40`, function/opcode/register fields, `pos=13`,
  `size=2`, exact disassembly, operation naming, and explicit distinction
  from the swapped `pos=1,size=14` word `0x7cc36840`.
- 1, 2, 8, 16, 31, and 32-bit widths; positions 0, 13, and 31; the bit-31
  boundary and `pos + size = 32`; zero, all-ones, positive, negative, and
  invalid non-sign-extended word sources; and no shift-by-32 case.
- Big-endian and little-endian instruction fetch and execution, PC/Count
  progression, source preservation, aliasing, source/destination `$zero`,
  taken and untaken branch delay slots, and no unrelated GPR, CP0, HI/LO,
  memory, or reservation changes.
- Typed range/source UNPREDICTABLE stops, no destination mutation, CPU
  serialization replay, DEXT/DEXTM distinction, and existing DEXT/DEXTM
  regression suites.
- Synthetic full-VM v3 snapshot replay in both byte orders with a live LL
  reservation and branch delay state, plus transactional rejection of a
  corrupted snapshot.

The CPU and full-VM formats remain unchanged: `GXM64SNP` v3, CPU v6 (1,001
bytes), memory v2, UART v4, and IRQ routing v1. EXT adds no persistent state.

## 6. Authentic EXT retirement

The runner replayed the 256-record accepted startup prefix and eight
post-Config0 instructions. It then fetched and decoded the actual instruction
through the production VM decoder at `0xffffffff8010b85c` (physical address
`0x00010b85c`). The pre-instruction state was:

| Observation | Value |
|---|---:|
| Raw guest bytes / word | `7cc30b40` / `0x7cc30b40` |
| `$a2` source | `0xffffffff8000c483` |
| Sign-extended 32-bit word check | valid (`signExtend32(0x8000c483)`) |
| `$v1` before | `0x0000000000000200` |
| Position / size | 13 / 2 |
| Independent expected result | `0x0000000000000002` |
| PC, Count, VM cycles, retired | `0xffffffff8010b85c`, 158,214, 158,214, 158,214 |
| Delay slot armed | no |

The production VM retired the EXT. `$v1` became `0x2`; PC advanced to
`0xffffffff8010b860`, and Count, VM cycles, and retired instructions each
advanced to 158,215. `$a2`, other GPRs, CP0 except Count, HI/LO, TLB, RAM, and
reservation state remained unchanged. No guest exception was entered.

## 7. Snapshot replay and authentic continuation

The focused synthetic VM tests cover pre/post replay, aliases, both byte
orders, defined and UNPREDICTABLE cases, and branch delay execution. For the
authentic run, the test captured the full VM immediately before EXT, retired
the instruction, restored the snapshot, and repeated it. It also restored the
same pre-EXT state and continued Linux to the identical stop. CPU state, RAM,
reservation generations, device state, UART/console, scheduler state, cycles,
and stop reason matched. Invalid snapshot restoration was rejected without
changing the live VM.

Linux retired 158,265 instructions in total (158,248 since Linux entry),
including 50 instructions after EXT. The next genuine frontier is at
`0xffffffff8010ba74`, fetch PA `0x00010ba74`, raw word `0x7ca3f806`. It is
SPECIAL3 function `0x06` (DINSU; LLVM prints the generic alias
`dins $3, $5, 0x20, 0x20`). Its unsupported attempt stopped the VM without a
guest exception; the final stop reason is `unsupported_instruction`. The
continuation budget was 1,000,000 attempts and was not exhausted.

At the frontier, Count and VM cycles were 158,265; Status was `0x400080`,
Cause/EPC/BadVAddr were zero, no delay slot was armed, no TLB entry was valid,
and IRQ lines 0–5 were inactive. UART TX/RX and console output remained zero.
MMIO was not instrumented, so no MMIO count or device conclusion is claimed.
The full prestate, retirement, bounded summary, and frontier are recorded in
the new JSONL trace.

## 8. Build, regressions, and production linkage

- Rebuilt all MIPS64 test executables and production `ia64emu`; the 362-step
  Ninja build completed successfully.
- MIPS64 CTest: **58/58 passed** (57 inherited suites plus `Mips64ExtTests`).
- Standalone focused counts: EXT **1,601/1,601**, Config0 **528/528**, and
  PRId **424/424**.
- Selected cross-ISA regressions: **6/6 passed** — BasicTests,
  NewInstructionTests, EfiMemoryMapTests, IA64SalTests, VMCheckpointTests,
  and RiscVDecoderSmokeTest.
- `nm -C build/bin/ia64emu.exe` contains production definitions for
  `Mips64Cpu::executeDecoded`, `step`, `disassemble`, and `opName`, confirming
  the changed decoder/executor, disassembly, and naming code is linked. The
  production executable also contains both EXT UNPREDICTABLE diagnostics and
  the updated SPECIAL3 unsupported diagnostic strings.

## 9. Limits and MIPS64-31 recommendation

This milestone qualifies EXT and the next authentic instruction boundary. It
does not claim full Linux boot, full 5KE equivalence, cache behavior, or
unobserved platform features. MMIO activity was not instrumented. The next
bounded phase should qualify the authenticated DINSU word `0x7ca3f806` using
its SPECIAL3 function `0x06`, preserving the current kernel, ROM/PROM, trace,
snapshot formats, and stop discipline.
