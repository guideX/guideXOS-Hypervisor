# MIPS64-14 — SLTIU Qualification and OpenWrt Linux Continuation

## Qualification result

**Outcome A — MIPS64-14 complete.** The authenticated OpenWrt Malta BE64
frontier was reproduced before the decoder change, `SLTIU` was implemented and
qualified using encoded guest instructions, the production VM retired the
kernel instruction, and the unchanged kernel advanced to a new unsupported
instruction. Full-VM snapshot replay and the regression gates passed. Linux
boot remains unverified.

The work started on branch `MIPS64` at
`7cbbda711a5b73182a6f68492d67e7f36695eaa6`. The upstream branch was aligned
(0 ahead, 0 behind), the worktree was clean, and `.phase-mips64` marked
MIPS64-13 complete and MIPS64-14 next. No branch or Git topology changes were
made. The MIPS64-12 and MIPS64-13 reports and the committed Phase 13 trace
were reviewed. The initial 39 MIPS64 CTest suites passed before source
changes.

## Kernel and boot configuration

The exact ignored kernel from Phase 12/13 was reused without modification:

| Property | Value |
|---|---|
| Path | `build/mips64-12-artifacts/openwrt-24.10.4-malta-be64-vmlinux.elf` |
| Distribution / target | OpenWrt 24.10.4 / Malta BE64 |
| Size | 11,013,776 bytes |
| SHA-256 | `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc` |
| ELF entry | `0xffffffff80917670` |
| Guest RAM | 256 MiB, big-endian |
| Boot | Existing MIPS64-12 Malta ROM and PROM handoff |

The inspected `E:\CodexArtifacts\guideXOS_Hypervisor` directory contained
unrelated build and architecture artifacts; the documented MIPS64-specific
artifact directory did not exist. The accepted kernel was present in the
repository's ignored build artifact directory. Its hash matched before both
the pre-change frontier reproduction and the Phase 14 replay.

The existing command line (`console=ttyS0,38400n8`), PROM values, generated
4 MiB Malta ROM, 256 MiB RAM, ELF load plan, and production `VirtualMachine`
setup were reused unchanged. No kernel image, ROM, PROM table, loader, or
device model was patched.

## Phase 13 frontier reproduction

Before changing the decoder, the production VM replayed the same kernel to
the Phase 13 frontier. The pre-change trace SHA-256 matched the accepted
Phase 13 trace (`bb17847d255eb838b1662b973cfb8a7f95ced6bd4ef5e5a00194a26a9063ec13`).

| Observation | Reproduced value |
|---|---|
| Retired instructions | 150,287 |
| Attempted steps | 150,288 |
| Retired from Linux entry | 150,270 |
| Progress beyond the earlier DSRL32 frontier | 246 |
| PC / physical address | `0xffffffff80b4f0b4` / `0x00b4f0b4` |
| Fetched word | `0x2c420021` |
| `$2` before the attempt | `0x0000000000000006` |
| CP0 Count / VM cycles | 150,288 / 150,287 after the unsupported attempt |
| UART | 0 bytes |

The VM had no architectural exception, TLB fault, or MMIO fault at this
frontier. CP0 Count advanced for the unsuccessful attempt while the VM
retirement count did not. Timer semantics were left unchanged.

## Independent SLTIU decode and architecture

The guest word was read from the fetched big-endian instruction stream and
independently decoded:

| Field | Value |
|---|---:|
| Raw word | `0x2c420021` |
| Primary opcode | `0x0b` (`SLTIU`) |
| `rs` | 2 |
| `rt` | 2 |
| Immediate field | `0x0021` = 33 |
| Disassembly | `sltiu $2, $2, 33` |

MIPS64 Release 2 sign-extends the 16-bit immediate and compares the full
register values as unsigned integers. The result is a full-width zero or one,
and the comparison raises no integer-overflow exception. The operation is:

```text
immediate64 = uint64_t(sign_extend_64(immediate16))
GPR[rt] = (uint64_t(GPR[rs]) < immediate64) ? 1 : 0
```

For the authenticated operands, `6 < 33`, so `$2` must become
`0x0000000000000001`. The specification was checked against the
[MIPS64 Architecture for Programmers, Volume II-A, Revision 5.04](https://wiki.chuang.ac.cn/_media/md00087-2b-mips64bis-afp-05.04.pdf),
SLTIU entry.

## Implementation and focused qualification

`Mips64Op::Sltiu` was appended to preserve existing operation enum values.
Opcode `0x0b` now decodes to that operation. Execution reads `GPR[rs]` before
writing `GPR[rt]`, sign-extends with the existing 16-bit helper, compares
both complete `uint64_t` values, and writes through the existing `$zero`
discard path. It does not touch HI/LO, CP0, the TLB, or memory. Existing
normal and branch-delay retirement sequencing is reused. Disassembly reports
the signed immediate value. CPU serialization and snapshot versions were not
changed.

`test_mips64_sltiu` is registered as `Mips64SltiuTests`. The focused suite
passed **3,992 assertions** over encoded instructions. It covers the
authenticated word; equal and less/greater comparisons; zero registers and
zero immediate; immediate boundaries 32767, -1 and -32768; `UINT64_MAX`, the
high bit, values above 32 bits, and unsigned full-width ordering; both endian
modes; `rs == rt`; destination `$zero`; branch-delay retirement; PC and Count;
unchanged unrelated GPRs, HI/LO, CP0, TLB and instruction memory; absence of
overflow exceptions; disassembly; CPU serialization; and unsupported opcode
behavior. The required negative-immediate examples passed:

```text
SLTIU source=0, immediate=-1           -> 1
SLTIU source=0xffffffffffffffff, -1    -> 0
```

The signed `SLTI` opcode (`0x0a`) remains outside the implemented subset and
still reports `UnsupportedInstruction`; it does not alias to the new
unsigned operation. Existing SLT, SLTU, ADDIU/ORI, DSRL32, J/JAL, branch, and
both-endian regressions passed in the inherited suite.

A full Malta VM snapshot test captures immediately before a valid encoded
SLTIU, executes it, restores, and reexecutes. The restored CPU serialization,
PC, registers, cycles, retirement count, instruction memory, full snapshotted
physical-memory regions, console, and UART state match. The test preserves
`GXM64SNP` v2, UART v4, IRQ routing v1, CPU serialization v5 (957 bytes).

## Authentic Linux retirement and snapshot replay

The production VM reproduced the earlier boot prefix, DSRL32 frontier and
SLTIU pre-state. Immediately before SLTIU, `$2=6`, CP0 Count and VM cycles
were both 150,287, Status was `0x00400080`, and Cause/EPC/BadVAddr were zero.
It fetched the unchanged word `0x2c420021` from physical `0x00b4f0b4`.

The VM retired SLTIU normally: `$2` became 1, PC advanced to
`0xffffffff80b4f0b8`, CP0 Count and VM cycles advanced to 150,288, and the
instruction-retirement count advanced by one. No exception, HI/LO change,
memory write, UART output, or VM error occurred.

A full VM snapshot was captured immediately before the authentic SLTIU. The
instruction was executed, the snapshot restored, and the same instruction
reexecuted. CPU serialization, register result, PC, Count, VM cycles,
retirement count, fetched RAM bytes, console output and UART transmit/time
state matched across both executions. Restore did not advance the CPU or
change the instruction bytes. Snapshot versions remained `2/4/1/5`.

## Bounded continuation and next frontier

The unchanged kernel continued from the SLTIU frontier with a deterministic
budget of 1,000,000 attempted guest steps after SLTIU (1,150,288 total
attempts from reset). It reached another authentic instruction frontier
before the budget expired:

| Observation | Value |
|---|---|
| Total retired from reset | 150,313 |
| Total attempted guest steps | 150,314 |
| Retired from Linux entry | 150,296 |
| Retired beyond the former SLTIU frontier | 26, including SLTIU; 25 subsequent instructions |
| Distinct guest PCs | 276 |
| Next PC / physical fetch | `0xffffffff8091a338` / `0x00091a338` |
| Raw word | `0xd0850000` |
| Independent disassembly | `lld $5, 0x0($4)` (LLVM 18.1.8) |
| Decoded fields | opcode `0x34`, `rs=4`, `rt=5`, immediate 0 |
| Source / destination-before | `$4=0xffffffff80b0d078`, `$5=1` |
| Stop | Typed `UnsupportedInstruction`; fetch succeeded from RAM |
| CP0 state at stop | Status `0x00400080`; Cause/EPC/BadVAddr zero |
| Valid TLB entries | 0 |
| UART output | 0 bytes |
| VM state | Fail-fast error state after the unsupported attempt |

The `LLD` word was decoded independently by LLVM 18.1.8 and its opcode and
operands were checked from the raw fields. The MIPS64 Volume II-A entry
identifies LLD as a doubleword load-linked operation that starts an atomic
read-modify-write sequence. It was not executed or implemented in this
phase. The failing decode caused no architectural exception, TLB fault, or
MMIO access. CP0 Count is 150,314 while VM retirement cycles are 150,313
because the unsupported LLD attempt advances Count; timer behavior is
unchanged. The guest produced no UART bytes and Linux boot is not claimed.

## Trace consistency

The new bounded trace is
[`docs/artifacts/openwrt_malta_be64_sltiu_phase14.jsonl`](artifacts/openwrt_malta_be64_sltiu_phase14.jsonl).
It contains 534 records, is 159,757 bytes, and has SHA-256
`f3748551aa3d53f8f18a66cda6cb700b2a47f9799fc846cf216f1f10deb693e0`.
It records the authenticated SLTIU operands and result, both snapshot replay
checks, the continued instructions, stop state, and next frontier.

The first 256 instruction records compare byte-for-byte with the accepted
Phase 13 trace; those Phase 13 records also compare byte-for-byte with the
Phase 12 trace. Both comparisons had zero mismatches. Earlier committed
traces were not changed. The pre-change replay was kept only in the ignored
build directory. The kernel hash, ROM/PROM setup, entry point, initial
prefix, DSRL32 result, and SLTIU pre-state all match the accepted evidence.

## Regression gates and linkage

- **MIPS64: 40/40 CTest suites passed**, including the 39 accepted suites and
  `Mips64SltiuTests`.
- **10,763 MIPS64 assertions passed:** 6,771 inherited Phase 13 assertions
  plus 3,992 SLTIU assertions.
- **Selected cross-ISA regressions: 6/6 passed:** `BasicTests`,
  `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`,
  `VMCheckpointTests`, and `RiscVDecoderSmokeTest`.
- Production `build/bin/ia64emu.exe` linked MIPS64 symbols for
  `mips64::decodeInstruction`, `Mips64Cpu::executeDecoded`, and
  `Mips64Cpu::step`; the focused SLTIU test executable linked and passed.
- The known separate IA-64 NaT assertion at
  `tests/test_isa_plugin.cpp:1551` and six historically unavailable CTest
  executables remain outside the selected regression set.
- No IA-64 production source or IA-64 `.phase` file changed.

## Limits and MIPS64-15 recommendation

The emulator still does not boot OpenWrt or produce UART output. No additional
instruction family or Malta device was added. The exact next guest frontier
is `LLD $5,0($4)` at `0xffffffff8091a338`.

Recommended MIPS64-15 scope: authenticate and qualify this exact LLD
operation, inspect the subsequent authentic stream for an SCD pairing, and
implement only the atomic reservation behavior required by observed guest
execution. Include snapshot/serialization handling for reservation state if
the chosen architectural model requires it. Continue the same kernel to the
next genuine frontier without broadening the instruction subset speculatively.
