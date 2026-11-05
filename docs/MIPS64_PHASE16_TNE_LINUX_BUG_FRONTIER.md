# MIPS64-16: TNE and the OpenWrt Linux BUG frontier

## Acceptance result

**Outcome A1 — TNE accepted; the authentic Linux comparison is false and execution continues.** The Phase 15 stop was an assertion-shaped TNE instruction, but the guest supplied `$a0 = 0`. TNE therefore retired normally. The same production-VM state was restored and replayed with identical CPU, cycle, Count, UART, and console results. Linux then retired 159 further instructions and stopped at the next unsupported MIPS64 instruction, `0x00e63004`, at `0xffffffff80b4ef34`. This does not establish Linux boot completion.

## Repository and kernel identity

- Checkout: `D:\dev\guideXOS_Hypervisor_MIPS64`
- Branch: `MIPS64`; starting HEAD was `7b7a88a92a9327f7365309a4b0db4ab7ef444d4c`; upstream divergence at preflight was `0 0`.
- The worktree was clean before this phase. The IA-64 `.phase` marker was left unchanged.
- `.phase-mips64` began with `last_completed=MIPS64-15` and `next_expected=MIPS64-16`.
- Kernel: OpenWrt 24.10.4 Malta BE64, ELF64 big-endian MIPS64r2, 11,013,776 bytes, entry `0xffffffff80917670`, 256 MiB guest RAM, existing ROM/PROM setup.
- Kernel SHA-256: `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc`.
- The kernel banner reports Linux 6.6.110. OpenWrt's 24.10.4 changelog records the 6.6.110 kernel update.
- The exact ELF is preserved at `build/mips64-12-artifacts/openwrt-24.10.4-malta-be64-vmlinux.elf`. No kernel image or PROM bytes were changed.

The new bounded execution trace is [`docs/artifacts/openwrt_malta_be64_tne_phase16.jsonl`](artifacts/openwrt_malta_be64_tne_phase16.jsonl), 736 JSONL records, 221,087 bytes, SHA-256 `b5fe371fe10adac89ecf7b5d449dcf302763196b0b45944077c865a9f092f`. Every line parses as JSON. It records 724 decoded instruction events, the TNE checkpoint, snapshot replay, LLD/SCD checkpoint, continuation window, and next frontier. The trace marks a four-attempt gap inside the continuation and a separate 149,785-instruction gap before the earlier Phase 13 DSRL32 frontier. From that frontier, 309 retired instructions reach TNE, and 469 reach the next frontier, providing a bounded 256–1,024-instruction context around the check. Earlier Phase 12–15 reports, traces, and hashes remain intact.

## Authenticated TNE frontier and Linux condition

The former Phase 15 frontier reproduced after 150,350 retired instructions:

| Field | Observed value |
|---|---|
| Guest PC | `0xffffffff801caf50` |
| Physical fetch | `0x001caf50` |
| Big-endian bytes / word | `00 04 03 36` / `0x00040336` |
| Fields | SPECIAL, `rs=0` (`$zero`), `rt=4` (`$a0`), code `12`, funct `0x36` |
| `$a0` | `0x0000000000000000` |
| `$zero` | `0x0000000000000000` |
| Status / Cause / EPC | `0x00400080` / `0` / `0` |
| Delay state | Not in a branch delay slot |
| Pre-implementation result | `UnsupportedInstruction` |

The pre-execution full-VM checkpoint is GXM64SNP v3, CPU component v6 (1,001 architectural bytes), memory v2, UART v4, and IRQ route v1. TNE required no additional serialized state and no version changes.

The ELF is stripped and has no usable function symbols. The address-based disassembly around the site is:

```text
0xffffffff801caf50: tne $zero, $a0, 12
0xffffffff801caf54: jr  $ra
```

The helper is called at `0xffffffff80b4ee58`; its return address is `0xffffffff80b4ee60`. The preceding caller instructions show the condition computation:

```text
0xffffffff80b4ee4c: lui    $a2, 0x80b1
0xffffffff80b4ee50: sltiu  $a0, $v0, -0xfff
0xffffffff80b4ee54: sltiu  $a0, $a0, 1
0xffffffff80b4ee58: jal    0xffffffff801caf50
0xffffffff80b4ee5c: daddiu $a2, $a2, -0x4618   # delay slot
```

At the frontier `$v0` is `0xffffffff80b0f380`. The first `sltiu` compares it unsigned against sign-extended `-4095` (`0xfffffffffffff001`), producing 1. The second compares 1 with 1, producing 0. Thus the final `$a0` is zero. This is the shape of an `IS_ERR_VALUE`-style check: the tested value is not in the Linux error-pointer range. The observed path through the preceding indirect call returns `0xffffffff80b0f380`; the return value is constructed by a `lui`/`daddiu` sequence in the callee, not loaded from guest memory.

The last write to `$a0` is the `sltiu` at `0xffffffff80b4ee54`. The condition's immediate dataflow is therefore established without assigning an unauthenticated source function name. No source line or containing function can be recovered from this stripped ELF.

The trace also records an earlier LLD/SCD pair: LLD read zero from physical `0x000b0d078`; SCD succeeded and stored `0xffffffff80a8fb40` there. The SCD result is not the source operand to TNE: subsequent code overwrites `$v0`, makes an indirect call, checks that call's returned pointer, and writes the final boolean to `$a0`. The bounded stream shows no direct register dataflow from the SCD success result to the TNE condition. It does not prove that every possible indirect memory or control dependency elsewhere in the kernel is unrelated.

The trace's preceding branch at `0xffffffff801dce90` tests the indirect call's zero argument and takes the path that returns the static pointer. The return sequence at `0xffffffff801dcf08`–`0xffffffff801dcf10` constructs `$v0 = 0xffffffff80b0f380`. The error-pointer predicate then evaluates false. No guest assertion is raised, so there is no earlier CPU, CP0, memory, firmware, or platform defect established by this check.

## Linux BUG_ON provenance

The exact OpenWrt 24.10.4 kernel is Linux 6.6.110. In that stable source, MIPS `__BUG_ON` uses a conditional `tne $zero, condition, BRK_BUG`; `BRK_BUG` is 12. Linux's trap handler recognizes that break code in the trap path. The binary's `tne $zero,$a0,12` therefore matches the Linux assertion mechanism, but the code value alone does not say whether the condition is true. Here it is false, so the BUG handler was not entered.

References:

- [OpenWrt 24.10.4 changelog](https://openwrt.org/releases/24.10/changelog-24.10.4)
- [Linux v6.6.110 MIPS BUG implementation](https://github.com/gregkh/linux/blob/v6.6.110/arch/mips/include/asm/bug.h)
- [Linux v6.6.110 MIPS break-code definitions](https://github.com/gregkh/linux/blob/v6.6.110/arch/mips/include/uapi/asm/break.h)
- [Linux v6.6.110 MIPS trap handling](https://github.com/gregkh/linux/blob/v6.6.110/arch/mips/kernel/traps.c)
- [GNU MIPS simulator instruction definitions at the referenced revision](https://gnu.googlesource.com/binutils-gdb/+/cbe950e9fe8a56f788bcf985cc51733f18abb45b/sim/mips/mips.igen)
- [MIPS64 Architecture for Programmers, Volume II, Revision 2.50](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf)

## Implementation and exception qualification

`Mips64Cpu` now decodes SPECIAL/funct `0x36` as TNE and extracts the complete 10-bit code from bits 15:6. The code is displayed in disassembly and does not constrain the comparison. Execution compares the complete 64-bit GPR values, performs no GPR write, retires normally on equality, and enters the existing synchronous exception path on inequality with `Cause.ExcCode = 13` and `Mips64Fault::TrapException`.

The focused encoded-instruction suite qualifies the exact Linux word, code values 0/12/1023, zero and nonzero operands, same and different registers, differences in either 32-bit half, both fetch endiannesses, normal advancement, GPR preservation, Count accounting, pending Cause bits, delay-slot BD/EPC, nested EXL/ERL behavior, and BEV vector selection. It also checks SYSCALL (8), BREAK (9), RI (10), and overflow (12) remain distinct.

For a taken Trap, direct tests observe:

- BEV=0 general vector `0xffffffff80000180`; BEV=1 general vector `0xffffffffbfc00380`.
- ExcCode 13, EXL set, and the faulting instruction's EPC when entering from EXL clear.
- A delay-slot TNE sets Cause.BD and EPC to the branch instruction.
- Existing EXL preserves the prior EPC; ERL remains set while the shared entry path sets EXL; pending Cause IP bits survive.
- TNE does not modify a GPR. The attempted instruction advances Count/VM cycles once but is not counted as retired.

The authentic guest takes the other path: no exception state changes, PC advances to `0xffffffff801caf54`, and TNE retires once. A full-VM snapshot before TNE was restored and replayed. The replay matched CPU serialization, guest retired count, CP0 Count, VM cycles, console bytes, and UART transmit/time counters. The authentic checkpoint uses the existing versions (GXM64SNP v3, CPU v6/1,001 bytes, memory v2, UART v4, IRQ v1). No snapshot format or console-restoration behavior changed.

## Authentic continuation and limits

The unchanged kernel/prompt configuration reproduced the 150,350-retirement Phase 15 prefix. The TNE checkpoint records 150,350 retired before and 150,351 after; the complete bounded run reports 150,510 retired instructions, 150,511 attempted steps, and 150,493 instructions after Linux ELF entry. The next stop is a genuine unsupported instruction:

```text
PC       0xffffffff80b4ef34
PA       0x000b4ef34
word     0x00e63004
fields   SPECIAL, rs=7, rt=6, rd=6, sa=0, funct=0x04
decode   UnsupportedInstruction
```

This encoding is variable word shift left (`SLLV`), a valid MIPS instruction not yet implemented by this core. At the stop, Status is `0x00400080`, Cause/EPC/BadVAddr are zero, the guest has no valid TLB entries, and UART output remains 0 bytes. The stop occurred after only 150,511 attempts, before the 350,000-attempt bound. The authentic Linux continuation is not claimed to have booted.

The next phase should authenticate and implement MIPS64r2 `SLLV`, including its MIPS64 result-extension semantics, then replay the same ELF and PROM to the next real frontier. If an unstripped Linux 6.6.110 image with verifiable build provenance becomes available, use it to map the address-based callers to source symbols; do not infer a function name from this stripped artifact.

## Verification

- Pre-edit inherited baseline: 42/42 MIPS64 CTest suites passed.
- Post-edit MIPS64 CTest: 43/43 passed, including `Mips64TneTests` and the existing LLD/SCD, MMU, exception, interrupt, snapshot, device, bootstrap, and ELF-handoff suites.
- Focused TNE executable: 90 checks passed, 0 failed.
- Selected IA-64 and RISC-V regressions: `BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`, and `RiscVDecoderSmokeTest`: 6/6 passed.
- Production `ia64emu.exe` target built. Binary strings contain `tne`, `TrapException`, and `TNE conditional Trap`, confirming the updated MIPS64 implementation is linked.
- The focused authentic-kernel replay and TNE snapshot replay succeeded; its next stop is the unsupported SLLV frontier above.
- Aggregate `all` build: started as a 1,442-action rebuild and was interrupted at action 669 after the required targets and tests had passed. No Winsock link error was emitted in the observed output; because the aggregate build did not complete, its overall status and the documented `debug_harness` link behavior remain inconclusive.

## Scope and known limitations

No kernel assertion was suppressed; no guest state was patched; no broad instruction family, firmware service, JIT, cache-coherence model, or IA-64 code was changed. Linux boot completion was out of scope. The ELF's lack of symbols limits attribution to address-level disassembly and the bounded dynamic dataflow above. MIPS64-17 begins at `0xffffffff80b4ef34` with `SLLV`.
