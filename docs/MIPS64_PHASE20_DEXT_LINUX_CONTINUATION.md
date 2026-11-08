# MIPS64-20 — DEXT Qualification and OpenWrt Linux Continuation

**Outcome: A — complete.** The authenticated Linux DEXT retired with the independently expected result, and the unchanged kernel continued to the next valid-but-unsupported instruction. Focused, inherited, cross-ISA, production-build, and snapshot-replay gates passed. Linux boot completion remains unverified.

## Preflight and kernel identity

- Checkout: `D:\dev\guideXOS_Hypervisor_MIPS64`; branch: `MIPS64`.
- Starting HEAD: `5ba7c4f451035090af3b6e087cc32a5ce9ca347f` (the requested commit).
- The initial worktree was clean; the branch was 1 commit ahead and 0 behind `origin/MIPS64`.
- During qualification, the `origin/MIPS64` tracking ref advanced to the same starting HEAD (`5ba7c4f`) with a reflog entry marked `update by push`; this task issued no push. The new Phase 20 commit remains local.
- `.phase-mips64` recorded MIPS64-19 complete and MIPS64-20 next. The IA-64 `.phase` marker was not changed.
- Reports MIPS64-12 through MIPS64-19 and the accepted Phase 19 trace were reviewed. All 47 inherited MIPS64 CTest suites passed before implementation.
- `E:\CodexArtifacts\guideXOS_Hypervisor` was inspected. Its Phase 19 build directory referenced a PPC64 checkout, so the authoritative local MIPS64 build and existing ignored kernel artifact were used.

The unchanged kernel is `build/mips64-12-artifacts/openwrt-24.10.4-malta-be64-vmlinux.elf`:

- OpenWrt 24.10.4, Malta BE64, Linux 6.6.110.
- Size: 11,013,776 bytes; entry: `0xFFFFFFFF80917670`; guest RAM: 256 MiB.
- SHA-256: `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc`.
- Malta ROM/PROM handoff configuration is unchanged. The kernel was not regenerated or patched.

Phase 19's `docs/artifacts/openwrt_malta_be64_di_ei_phase19.jsonl` remains 77,665 bytes with SHA-256 `159f4b8ebe94f3fa1df82340d8f08812b03d5e9a0978812aeea01175dc5484b3`. The first 256 instruction records in the Phase 20 trace are byte-for-byte identical to that trace.

Phase 19 counted the unsupported DEXT fetch as attempt 151,585 after 151,584 retirements. The Phase 20 pre-execution record reports 151,584 attempts *before* executing DEXT; this is the same instruction boundary with the attempt counter captured before the now-supported instruction is stepped.

## DEXT implementation and encoding policy

The decoder recognizes SPECIAL3 opcode `0x1F` and DEXT function `0x03`. For DEXT, the generic decoded `rd` field is MSBD (`size - 1`), `sa` is LSB/position, and `rt` remains the destination. The new `Mips64Op::Dext` was appended to preserve existing operation values. Execution reads the full 64-bit RS before writing RT, uses a logical shift and a 1–32-bit unsigned mask, and writes the zero-extended result. Disassembly prints `dext rt, rs, pos, size`.

For DEXT, every five-bit `pos` and `msbd` combination is defined: `0 <= pos < 32`, `1 <= size <= 32`, and the highest source bit is at most 62 (`pos + size <= 63`). The Rev. 2.50 manual explicitly says DEXT has no UNPREDICTABLE case because of these field limits. Thus there is no out-of-range DEXT bit pattern to turn into a fabricated result or a fabricated RI. DEXTM and DEXTU remain unsupported; recognized allocated SPECIAL3 functions remain unsupported unless implemented, while unallocated function values remain reserved.

The SPECIAL function `0x14`, `0x16`, and `0x17` encodings were also corrected to remain valid-but-unsupported (DSLLV, DSRLV, and DSRAV) instead of being treated as reserved. This classification is needed to stop at the authenticated next frontier without manufacturing an exception; no shift instruction was implemented in this phase.

The implementation follows the [MIPS64 Instruction Set Manual, Revision 2.50](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf), especially DEXT pp. 132–133 and DSLLV p. 167. GNU Binutils' [MIPS opcode definitions](https://gnu.googlesource.com/binutils-gdb/+/refs/heads/master/include/opcode/mips.h) independently documents the SPECIAL3 field layout and DEXT operand limits.

## Focused qualification

The new `test_mips64_dext` CTest target executes real instruction words in little- and big-endian mode. It covers the exact Linux word, decode fields, independent extraction examples, field widths and positions, a field crossing bit 31, high-bit/all-one/zero sources, zero extension, aliases, `$zero`, delay-slot retirement, unrelated GPR/CP0/HI/LO/TLB/memory/reservation state, neighboring SPECIAL3 classifications, serialization, and production VM snapshot replay.

The production VM test first establishes an active LL reservation, captures a GXM64SNP snapshot before DEXT, executes DEXT, restores, and re-executes. CPU state, data and instruction memory, reservation state and generations, VM cycles, console bytes/sequence, and UART timing match after replay in both endian modes. CPU serialization remains v6 (1,001 bytes); no format change was needed.

## Authenticated Linux boundary and result

The production VirtualMachine reproduced the accepted Phase 19 prefix and the exact pre-DEXT frontier:

| State | Observed value |
|---|---|
| Retired / attempted before DEXT | 151,584 / 151,584 |
| PC VA / fetch PA | `0xFFFFFFFF8012F5A8` / `0x0012F5A8` |
| Raw instruction | `0x7C83C983` |
| Independent decode | SPECIAL3 `0x1F`, funct `0x03`; `rs=4`, `rt=3`, `msbd=25`, `lsb=6` |
| Assembly operation | `DEXT $v1,$a0,6,26` |
| `$a0` / previous `$v1` | `0x0000000000000000` / `0x0000000000000000` |
| Independent result | `(0 >> 6) & 0x03FFFFFF = 0` |
| Status / Cause | `0x0000000000400080` / `0` |
| Count / VM cycles | `151584` / `151584` |
| EPC / ErrorEPC / BadVAddr | all zero |
| Delay slot / LL reservation | not armed / invalid |
| Snapshot before DEXT | valid GXM64SNP v3; CPU v6, memory v2, UART v4, IRQ routing v1 |

DEXT retired normally: `$v1` became zero, PC advanced to `0xFFFFFFFF8012F5AC`, and Count and VM cycles each advanced once to 151,585. Status, Cause, EPC, ErrorEPC, BadVAddr, HI/LO, delay state, and reservation state remained unchanged. The runner restored the pre-DEXT full snapshot and reproduced the same retirement. It then restored that same snapshot again, replayed the unchanged continuation, and matched the full continuation state and stop reason, including CPU, memory contents, reservation generations, scheduler, FPGA, UART, and console bytes/sequence. The console's monotonic generation counter advances on snapshot restore, so replay equality compares the guest-visible console output and sequence instead.

## Next frontier and bounded stop

The next instruction is the valid MIPS64 DSLLV word `0x00832014` at VA `0xFFFFFFFF8012F5C8`, PA `0x0012F5C8`. Its fields are SPECIAL (`opcode=0`), `rs=4`, `rt=3`, `rd=4`, `sa=0`, `funct=0x14`; independently, the MIPS64 manual identifies this as `DSLLV rd,rt,rs`, or `dsllv $a0,$v1,$a0`. It is in a branch delay slot. The emulator reports `UnsupportedInstruction`, leaves the instruction unexecuted, and does not enter an architectural exception.

The bounded continuation therefore stopped at this authentic unsupported frontier before reaching the optional one-million-attempt limit:

- Total: 151,592 retired instructions and 151,593 attempts.
- Since Linux entry: 151,575 retired instructions (17 instructions retired before the kernel entry point).
- Beyond the former DEXT PC: 8 retired instructions total (DEXT plus 7 following instructions); 7 instructions retired after DEXT.
- Stop: `unsupported_instruction` at DSLLV; Count and VM cycles 151,592; Status `0x0000000000400080`; Cause/EPC/ErrorEPC/BadVAddr zero.
- UART output/transmits/receive bytes: 0 / 0 / 0; no interrupt delivery or device fault.
- Final full-VM snapshot: valid v3; 16 TLB entries; serialized CPU state 1,001 bytes. Replay produced the same state and stop.

The new trace, `docs/artifacts/openwrt_malta_be64_dext_phase20.jsonl`, contains 263 valid JSONL records (79,633 bytes), with the first 256 instruction records matching Phase 19 exactly. SHA-256: `7be851435c0a11e470bc41658297074dc4881cbc6bbcbe0b5f555bd098c8c01c`. Earlier reports and traces are preserved.

## Regression and production evidence

- MIPS64 CTest: **48/48 suites**, **21,367 assertions passed**, 0 failed; this includes all 47 inherited suites and `Mips64DextTests`.
- Focused DEXT: **4,355 passed**, 0 failed.
- Focused authentic DI/EI suite: 681 passed, 0 failed. ELF handoff CTest: 298 passed, 0 failed.
- Selected IA-64/RISC-V regressions (`BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`, `RiscVDecoderSmokeTest`): **6/6 passed**.
- Production `ia64emu.exe`, `test_mips64_dext`, and `test_mips64_elf_handoff` built successfully. The linked production image contains `Mips64Cpu::executeDecoded`, `step`, `disassemble`, and `opName`, which include DEXT execution and disassembly.
- Kernel SHA-256 and the Phase 19 trace SHA-256 remained unchanged. The Phase 19 DI boundary and its accepted result were reproduced in the new run.

## Limits and MIPS64-21 recommendation

Full Linux boot was not reached or claimed. DEXTM, DEXTU, DSLLV, DSRLV, DSRAV, and other unimplemented instruction families remain unsupported. The next phase should qualify the authenticated DSLLV delay-slot boundary and continue the unchanged kernel from it. No guest binary, PROM value, or Linux-specific register behavior was modified.
