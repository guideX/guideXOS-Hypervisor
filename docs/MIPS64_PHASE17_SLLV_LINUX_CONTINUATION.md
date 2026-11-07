# MIPS64-17: SLLV qualification and OpenWrt Linux continuation

## Acceptance result

**Outcome A — SLLV is implemented and accepted by architectural, production-VM, snapshot-replay, and authentic-kernel checks.** OpenWrt Linux retired the previously unsupported SLLV and continued for 61 more instructions. The bounded run then stopped at the next unsupported instruction, `0xc2020060`, at `0xffffffff801cbfd4`. This phase does not establish Linux boot completion.

## Repository and kernel identity

- Checkout: `D:\dev\guideXOS_Hypervisor_MIPS64`
- Branch: `MIPS64`; starting HEAD was `13ea1fec333c07d4335f45a3ec2c78f7b1c2783d`; upstream divergence at preflight was `0 0`.
- The worktree was clean before this phase. `.phase-mips64` began with `last_completed=MIPS64-16` and `next_expected=MIPS64-17`. The IA-64 `.phase` marker was left unchanged.
- Kernel: OpenWrt 24.10.4 Malta BE64, ELF64 big-endian MIPS64r2, 11,013,776 bytes, entry `0xffffffff80917670`, 256 MiB guest RAM, existing ROM/PROM setup.
- Kernel SHA-256: `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc`.
- The exact ELF remains at `build/mips64-12-artifacts/openwrt-24.10.4-malta-be64-vmlinux.elf`; no kernel image or PROM bytes were changed.

The new bounded execution trace is [`docs/artifacts/openwrt_malta_be64_sllv_phase17.jsonl`](artifacts/openwrt_malta_be64_sllv_phase17.jsonl), 786 JSONL records (771 instruction records), 236,503 bytes, SHA-256 `bc888a3d662cf9269aad197328091b3d87f3aee8d310cd5969b87b4d82025684`. The first 256 instruction records match the accepted Phase 16 trace byte-for-byte. The Phase 16 artifact remains unchanged (SHA-256 `b5fe371fe10adac89ecf7b5d449dcf302763196b0b45944077c865a9f092f`). The Phase 17 trace labels a 19-attempt gap in its bounded instruction stream; its checkpoint and aggregate counters are recorded separately.

## Architectural qualification

The accepted Linux word is `0x00e63004`, fetched at guest PC `0xffffffff80b4ef34` and physical address `0x000b4ef34`. It decodes as SPECIAL/funct `0x04`, `rs=7`, `rt=6`, `rd=6`, `sa=0`: `sllv $6, $6, $7`. The pre-instruction values are `$6=1` and `$7=0`.

MIPS64 Volume II specifies SLLV as a word shift: use the low 32 bits of the value, use bits 4:0 of the shift register, and sign-extend the 32-bit result to the 64-bit GPR. The `sa` field is reserved and must be zero. Here the effective shift is `0 & 31 = 0`; the word result is `0x00000001`, sign-extended result `0x0000000000000001`. The implementation rejects nonzero `sa` with Reserved Instruction and does not alter unrelated state. See [MIPS64 Architecture for Programmers, Volume II, Revision 2.50, SLLV](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf), pp. 324–325.

The focused `Mips64SllvTests` suite contains **5,065 checks**. It covers the exact Linux encoding and disassembly; shift counts 0, 1, 7, 15, 16, 31 and high-count masking; truncation to 32 bits and sign extension; aliasing among `rs`, `rt`, and `rd`; zero-register cases; both memory endiannesses; delay-slot execution; reserved-field rejection; normal PC/Count retirement; unrelated GPR/CP0/HI/LO/TLB preservation; serialization; and production Malta VM snapshot behavior.

## Authentic SLLV state and replay

| Field | Before SLLV | After one SLLV |
|---|---:|---:|
| Guest PC | `0xffffffff80b4ef34` | `0xffffffff80b4ef38` |
| Instruction | `0x00e63004` | Retired |
| `$6` | `0x0000000000000001` | `0x0000000000000001` |
| `$7` | `0x0000000000000000` | unchanged |
| CP0 Count / VM cycles / retired | `150510` | `150511` |
| Status | `0x00400080` | unchanged |
| Cause / EPC / BadVAddr | `0 / 0 / 0` | unchanged |
| Delay slot / MIPS fault | no / None | no / None |

The full-VM checkpoint uses GXM64SNP v3, CPU component v6 (1,001 architectural bytes), memory v2, UART v4, and IRQ route v1. Restoring and re-executing SLLV reproduces its CPU and VM state. A second restore replays 62 successful steps including SLLV (61 following retirements) and reaches the same next frontier. CPU, memory, reservation generations, counters, UART, and console state match the original continuation. SLLV has no guest memory, console, or UART transmit effects; the UART virtual-time counter advances with the normal VM cycle.

The Phase 16 TNE checkpoint is still authenticated on its first visit. Supporting SLLV lets Linux later revisit that same TNE address, so the harness now validates the Phase 16 checkpoint only once and executes later visits normally.

## Authentic Linux continuation and next frontier

The run reproduces the SLLV prestate after 150,510 retired instructions. SLLV retires, followed by 61 additional successful instructions. The next stop is:

```text
PC       0xffffffff801cbfd4
PA       0x0001cbfd4
word     0xc2020060
decode   UnsupportedInstruction (opcode 48, rs=16, rt=2, immediate=0x0060)
```

At the stop, the guest has retired 150,572 instructions in 150,573 attempts; 150,555 instructions retired after Linux ELF entry. CP0 Count and VM cycles are both 150,572. Status is `0x00400080`; Cause, EPC, and BadVAddr are zero; no delay slot is armed; there are no valid TLB entries; UART output is 0 bytes; and the MIPS fault is `None`. The instruction is an unsupported COP2 load encoding (`LWC2`); its guest use and required coprocessor behavior remain for the next phase to establish. No behavior was added speculatively here.

The trace records earlier authenticated DSRL32, SLTIU, LLD/SCD, and TNE checkpoints as well as the SLLV execution and next stop. No guest register or memory state was patched, and no guest exception was suppressed.

## Verification

- Full MIPS64 CTest run: **44/44 suites passed, 16,096 checks passed, 0 failed**, including the new SLLV suite and all inherited CPU, exception, MMU, VM, Malta, ELF, snapshot, interrupt, and UART suites.
- Selected IA-64 and RISC-V regression suites: `BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`, and `RiscVDecoderSmokeTest`: **6/6 passed**.
- Production `ia64emu.exe` and `ia64emu_tests.exe` targets built. `nm -C` confirms the production executable contains `Mips64Cpu::executeDecoded`, `Mips64Cpu::opName`, and `Mips64Cpu::disassemble`.
- Authentic SLLV instruction replay and 62-step full-VM continuation replay both matched. The first 256 instruction records matched Phase 16 exactly.
- The kernel and Phase 16 accepted trace were not modified.

## Next phase

MIPS64-18 should investigate the `0xc2020060` COP2 load at `0xffffffff801cbfd4` from the authentic guest state. Establish whether the kernel expects Malta COP2 state or a particular MIPS64 coprocessor behavior, then implement only the demonstrated architectural path and continue the same unmodified ELF/PROM to the next frontier.

## Erratum recorded in MIPS64-18

The preceding frontier narrative is incorrect: `0xc2020060` has primary opcode `0x30` and is `LL $v0,96($s0)`, not LWC2. The Phase 17 JSON trace already records opcode decimal 48, `rs=16`, `rt=2`, and immediate `0x0060`; the trace itself is preserved unchanged. The LWC2 label was introduced only in this report text. See [MIPS64-18 LL/SC word atomics](MIPS64_PHASE18_LL_SC_WORD_ATOMICS.md) for the authentic memory operands, four-byte reservation, SC pairing, replay, and next frontier. A genuine LWC2 opcode is `0x32` and remains distinct.
