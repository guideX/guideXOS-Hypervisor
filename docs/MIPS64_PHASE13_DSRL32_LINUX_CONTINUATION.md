# MIPS64-13 — DSRL32 Qualification and OpenWrt Linux Continuation

## Qualification result

**Outcome A — MIPS64-13 complete.** The authenticated MIPS64-12 Linux frontier
was reproduced, the encoded DSRL32 was independently decoded and implemented,
the production VM retired it, and the unchanged kernel advanced to a new,
typed unsupported-instruction frontier. Snapshot compatibility and the
required regression gates passed. Linux boot remains unverified.

The work started on branch `MIPS64` at
`b41b1c1f60e39005c6fd147e642a9f97cb676229`. The worktree was clean,
`.phase-mips64` expected MIPS64-13. The branch was one local commit ahead of
`origin/MIPS64` at `9bee5d9fe2fbe3f3ef18fe2f203d2a05be6d144d`; its sole local
commit was `b41b1c1f60e39005c6fd147e642a9f97cb676229` (MIPS64-12), and it was
retained. No branch, worktree or other Git topology changes were made. The
IA-64 `.phase` file was not modified.

## Accepted kernel and Phase 12 setup

The exact ignored artifact used by MIPS64-12 was reused:

| Property | Value |
|---|---|
| Path | `D:\dev\guideXOS_Hypervisor_MIPS64\build\mips64-12-artifacts\openwrt-24.10.4-malta-be64-vmlinux.elf` |
| Size | 11,013,776 bytes |
| SHA-256 | `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc` |
| Format | ELF64, big-endian, `EM_MIPS`, static executable, MIPS64 Release 2 |
| Entry | `0xffffffff80917670` |
| Load segment | file offset `0x1000`, VA/PA `0xffffffff80100000`, file size `0xa7f9b0`, memory size `0xac93d4`, RWX, 4 KiB alignment |
| Provenance | OpenWrt 24.10.4 `malta/be64` release; `config.buildinfo` selects `CONFIG_TARGET_malta_be64=y` |

The SHA-256 matches the image listed by the [OpenWrt Malta BE64 release
index](https://downloads.openwrt.org/releases/24.10.4/targets/malta/be64/).
The existing 256 MiB big-endian guest RAM, generated 4 MiB Malta ROM, normal
ROM-to-ELF transfer, and PROM environment from Phase 12 were used unchanged.
The command line was `console=ttyS0,38400n8`; the PROM supplied `$a0=2`,
`$a1=0xffffffff80002000`, `$a2=0xffffffff80002008`, and `$a3=268435456`.
The release [build configuration](https://downloads.openwrt.org/releases/24.10.4/targets/malta/be64/config.buildinfo)
identifies the target. No kernel was downloaded, rebuilt or patched for this
phase.

The accepted Phase 12 probe retired 150,041 instructions and stopped at
`0xffffffff80917700`. Its committed trace contains 256 initial instruction
records, a bounded gap record and the unsupported frontier. Before editing,
the same artifact and production VM reproduced that count, PC and raw word.
At that point `$12=0`, Status was `0x00400080`, Cause/EPC/BadVAddr were zero,
there were no valid TLB entries, and UART output was empty.

## DSRL32 encoding and implementation

The raw kernel word is **`0x000c643e`**, fetched big-endian as bytes
`00 0c 64 3e`. Its fields are:

| Field | Value |
|---|---:|
| Primary opcode | `SPECIAL` (`0x00`) |
| `rs` | `0` |
| `rt` | `12` |
| `rd` | `12` |
| `sa` | `16` |
| Function | `0x3e` |
| Effective shift | `sa + 32 = 48` |

LLVM 18.1.8 `llvm-objdump` independently disassembled the kernel bytes as
`dsrl32 $12, $12, 0x10`. MIPS64 Release 2 defines this operation as a
64-bit logical right shift with zero fill:

```text
GPR[rd] = uint64(GPR[rt]) >> (sa + 32)
```

The instruction-set manual assigns `rs=0` to DSRL32 and `rs=1` to DROTR32;
the other `rs` encodings are reserved for this function. DROTR32 remains
explicitly unsupported and reserved encodings remain reserved. The manual's
DSRL32, DROTR32, J and JAL entries are in the [MIPS64 Volume II-A, Revision
5.04](https://wiki.chuang.ac.cn/_media/md00087-2b-mips64bis-afp-05.04.pdf)
instruction reference; the [LSU MIPS64 Release 2 reference](https://www.ece.lsu.edu/ee4720/mips64v2.pdf)
is supplementary.

The source register is read before writing the destination. The shift amount
is always between 32 and 63, so the `uint64_t` shift is defined. The result is
not sign-extended. Register zero keeps its normal write-discard behavior.
`Mips64Op::Dsrl32` was appended to the operation enum to preserve existing
enum values. Decode, execute, disassembly and operation naming use the
existing CPU paths; no snapshot or CPU serialization version changed.

### Necessary jump-target correction

The authentic stream immediately after DSRL32 contains `J 0x0b42678` at
`0xffffffff80917714`. That exposed a separate, concrete correctness error in
the existing J/JAL pseudodirect target mask: it preserved only the top
nibble. MIPS64 preserves `PC+4[63:28]` and takes the low 28 target bits from
the instruction index. The old mask therefore formed the invalid target
`0xf000000000b42678`; the corrected mask reaches the kernel's actual target
`0xffffffff80b42678` and honors the delay slot. A focused encoded J/JAL test
checks the upper address bits, delay-slot execution, JAL link value and target
fetch. This correction was necessary to follow the real kernel stream; no
additional instruction family was added.

## Focused and snapshot tests

`test_mips64_dsrl32` is registered as `Mips64Dsrl32Tests`. It passed **2,095
assertions**. Cases cover shifts 32, 33, 47 and 63; zero, all-one, high-bit and
mixed operands; full-width zero filling; `rd == rt`; `$zero` as source and
destination; little- and big-endian fetch; PC and retirement; unrelated GPR,
CP0, HI/LO, TLB and instruction-memory preservation; branch delay execution;
DROTR32 and reserved encodings; existing SLL/SRL/SRA and DSLL/DSRL/DSRA
behavior; and CPU serialization after DSRL32.

The focused Malta VM test captures immediately before DSRL32, executes it,
restores, and reexecutes it. The pre- and post-replay CPU state, cycle and
retirement counts, RAM, console and UART results match. The snapshot remains
`GXM64SNP` v2 with UART v4, IRQ routing v1 and CPU v5.

The authentic kernel replay also checkpoints at the former frontier using
the production full-VM snapshot. The snapshot passed integrity checks with
versions `2/4/1/5`. After restore, the kernel fetched the same instruction
bytes and reexecuted DSRL32 to the identical serialized CPU state, cycle and
retirement counts, console bytes and UART transmit/time state. The in-memory
snapshot was not persisted to disk.

## Production Linux continuation

The VM reproduced the 150,041-retirement Phase 12 prefix and verified the
former frontier's PC, raw word, decoded fields and pre-instruction state. At
the authentic kernel instruction, the independently calculated result is
`0x0000000000000000 >> 48 = 0x0000000000000000`. The production CPU retired
DSRL32, left `$12` at zero, advanced PC to `0xffffffff80917704`, incremented
Count from 150,041 to 150,042, and preserved Status, Cause, EPC, BadVAddr,
HI/LO and TLB state. It then retired the following kernel instructions,
including the corrected J and its delay slot.

The run allowed **1,000,000 additional attempted steps** beyond the Phase 12
frontier (a total cap of 1,150,041 attempts from reset). It stopped at the
first subsequent unsupported instruction, as required by the fail-fast
policy:

| Observation | Value |
|---|---|
| Retired from reset | 150,287 |
| Retired from Linux entry | 150,270 (17 ROM instructions preceded entry) |
| Retired beyond the old frontier | 246, including DSRL32 |
| Distinct guest PCs observed | 250 |
| Next PC / physical fetch | `0xffffffff80b4f0b4` / `0x00b4f0b4` |
| Raw word / independent disassembly | `0x2c420021` / `sltiu $2, $2, 0x21` |
| Stop classification | `UnsupportedInstruction`, valid MIPS64 opcode outside the implemented subset |
| Relevant state | `$2=6`, CP0 Count 150,288, VM cycles 150,287, Status `0x00400080`, Cause/EPC/BadVAddr zero, no valid TLB entries |
| VM/device observations | VM fail-fast error state; fetch succeeded from RAM; no bus or MMIO fault observed |
| UART | 0 bytes |
| Elapsed time | Approximately 4.7 seconds including the authentic snapshot replay |

The next frontier is a valid SLTIU instruction that this implementation does
not yet support. It is recorded for MIPS64-14 and was not implemented here.
The VM's error state and log message reflect strict fail-fast handling of the
typed unsupported instruction; they are not a MIPS architectural exception.
Count includes the failed instruction attempt, while VM retired cycles do not.
Linux boot, UART initialization and userspace are not claimed.

## Trace and replay integrity

The new bounded trace is
[`docs/artifacts/openwrt_malta_be64_dsrl32_phase13.jsonl`](artifacts/openwrt_malta_be64_dsrl32_phase13.jsonl).
It contains 506 JSONL records and is 150,732 bytes (SHA-256
`bb17847d255eb838b1662b973cfb8a7f95ced6bd4ef5e5a00194a26a9063ec13`). It
retains the first 256 instruction records, a pre-frontier skipped-count
record, the DSRL32 instruction and before/after state, the continued
instruction sequence through the next frontier, and the final stop record.
The 256 available instruction records compare byte-for-byte with the
committed Phase 12 trace: **zero mismatches**. No claim is made about
instruction-by-instruction identity inside the earlier skipped region.

The three committed Phase 12 traces were not changed. No guest image or
unbounded trace was added to the repository.

## Regression gates and production linkage

- **MIPS64: 39/39 CTest suites passed.** This is the 38 inherited suites plus
  `Mips64Dsrl32Tests`.
- **6,771 MIPS64 assertions passed:** 4,676 inherited plus 2,095 focused
  DSRL32 assertions.
- **Selected cross-architecture regressions: 6/6 passed:** `BasicTests`,
  `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`,
  `VMCheckpointTests` and `RiscVDecoderSmokeTest`.
- Production `build/bin/ia64emu.exe` linked MIPS64 symbols for
  `mips64::decodeInstruction`, `Mips64Cpu::executeDecoded` and
  `Mips64Cpu::step`; the focused new test executable also linked.
- No IA-64 source or `.phase` file changed. The known historical IA-64 NaT
  assertion at `tests/test_isa_plugin.cpp:1551` and six unavailable legacy
  CTest executables remain separate from this passing selected regression
  set.

## Limits and MIPS64-14 recommendation

The MIPS64 subset remains intentionally bounded. The kernel now reaches
`SLTIU` word `0x2c420021` at `0xffffffff80b4f0b4`; it does not yet boot or
produce UART output. The ELF loader, Malta device model, firmware behavior
and snapshot formats were not broadened.

Recommended MIPS64-14 scope: independently qualify the exact SLTIU encoding,
operand width and result semantics at this recorded frontier, add focused
decoder/execution regressions, then replay the same hashed kernel through the
production VM and stop at the next genuine frontier.
