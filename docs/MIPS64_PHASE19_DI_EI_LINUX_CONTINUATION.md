# MIPS64-19 — DI/EI Architectural Interrupt Control and Linux Continuation

**Outcome: A — complete.** DI/EI and their interrupt-boundary behavior are implemented and qualified. The unchanged OpenWrt kernel retired through the former DI frontier and stopped at the next valid but unsupported instruction. No full Linux boot is claimed.

## Repository preflight

- Checkout: `D:\dev\guideXOS_Hypervisor_MIPS64`
- Branch: `MIPS64`
- Starting commit: `2018e7120910275a457fa4ff491fb6f5a080fa33`
- Initial worktree: clean; upstream divergence: 0 ahead / 0 behind; local commits: 0.
- `.phase-mips64` initially recorded MIPS64-18 complete and MIPS64-19 next. The IA-64 `.phase` marker was left unchanged.
- MIPS64-12 through MIPS64-18 reports and the accepted Phase 18 trace were reviewed. The Phase 18 trace remains preserved at `docs/artifacts/openwrt_malta_be64_ll_phase18.jsonl`.
- The requested `E:\CodexArtifacts\guideXOS_Hypervisor` tree was inspected. It contains no copy of the authenticated OpenWrt Malta BE64 ELF; the working image remains the existing ignored build artifact. No external artifact was copied or changed.
- The accepted Phase 18 baseline passed 46/46 MIPS64 CTest suites (16,333 assertions), five selected IA-64 regressions, the RISC-V decoder smoke test, and the production `ia64emu.exe` build.

## Kernel identity and Phase 18 frontier

The unchanged image is `build/mips64-12-artifacts/openwrt-24.10.4-malta-be64-vmlinux.elf`:

- OpenWrt 24.10.4, Malta BE64, Linux 6.6.110
- Size: 11,013,776 bytes
- SHA-256: `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc`
- Entry: `0xFFFFFFFF80917670`; guest RAM: 256 MiB
- ROM/PROM handoff configuration and guest image are unchanged.

Phase 18 stopped before executing the valid instruction at VA `0xFFFFFFFF80B426C0`, PA `0x00B426C0`: `0x41606000` (`DI $zero`). Its pre-instruction counters were 151,572 retired and 151,573 attempts total, with 151,555 guest instructions retired since Linux entry. The Phase 19 run reproduced that exact boundary.

## DI/EI implementation

The decoder extends the existing COP0 MFMC0 path. It accepts only the DI/EI family with `rd=Status`, `sa=0`, reserved bits zero, and the selection bit choosing DI or EI; all 32 `rt` values are accepted. Other MFMC0 words remain reserved. Malformed encodings fault without changing Status. The exact Linux DI and matching EI words are `0x41606000` and `0x41606020`.

Execution captures the prior low 32 bits of Status, sign-extends that value into `GPR[rt]`, and changes only Status.IE. `$zero` discards the return value. Cause/IP, EPC, BadVAddr, Count/Compare, interrupt pins, and the CPU-local LL/SC reservation are not acknowledged or cleared by DI/EI. Normal instruction retirement advances PC, Count, and VM time once.

Kernel execution uses the existing CP0 privilege gate. User-mode DI/EI is rejected before either Status or the destination GPR changes. This core does not implement the CU0 access-enable model, so user-mode rejection follows the existing `PrivilegeViolation`/Reserved Instruction path rather than reporting the manual's Coprocessor Unusable exception. The limitation is explicit and no CP0 access is silently allowed.

## Interrupt ordering and hazards

The CPU is an in-order interpreter that commits each instruction's architectural state before the next instruction-boundary interrupt sample. DI/EI change Status.IE atomically during execution, so the next boundary sees the new value. An interrupt already eligible at DI's sampling boundary is delivered before DI. A pending interrupt after EI is delivered at the next boundary, which can be before the following EHB. The EI executor itself never injects an interrupt.

EHB (`0x000000C0`) is decoded and disassembled as its own Release 2 operation. It retires without register, memory, or CP0 changes. Because the interpreter has no in-flight pipeline state, every instruction boundary already provides the serialization this model supports; the implementation does not add a fixed delay or deferred IE state. JR.HB/JALR.HB are likewise named explicitly and retain the core's normal branch and delay-slot behavior. Ordinary SLL and SSNOP encodings are unchanged.

Boundary tests cover an already eligible interrupt before DI, a request pending while IE is clear, immediate Status.IE visibility after EI, delivery before a following EHB, EXL/ERL suppression, IM masking, and independent timer IP7 and Malta UART IP4 behavior. EHB tests verify normal retirement and no unrelated state mutation.

Snapshot formats are unchanged: CPU serialization remains v6 (1,001 bytes), and full VM snapshots remain GXM64SNP v3 with CPU component v6. Memory v2, UART v4, and IRQ routing v1 are unchanged. Status.IE is already serialized, so snapshots before DI/EI and immediately after either instruction replay without new hazard state. Tests cover full-VM replay before DI and EI, invalid restore without mutation, and matching CPU/device state after replay.

The architecture reference is the [MIPS64 Instruction Set Manual, Revision 5.04](https://s3-eu-west-1.amazonaws.com/downloads-mips/documents/MD00087-2B-MIPS64BIS-AFP-05.04.pdf): DI/EI and their execution hazard (pp. 144, 180), EHB (p. 179), and JR.HB/JALR.HB (pp. 194–195). This interpreter uses the task-permitted immediately serialized architectural model instead of simulating physical pipeline latency.

## Production guest demonstrations

- **Timer:** encoded Malta guest programs run in both endian modes. They configure IM7 and Compare, execute DI/EHB, observe TI/IP7 pending while IE is clear, then execute EI. At the next boundary, the pending timer enters a real guest handler before the following EHB; the handler acknowledges with Compare, returns by ERET to execute EHB, and publishes the resumed marker. Pre-DI and pre-EI full snapshots replay to the same CPU/device boundary.
- **UART:** the production VM configures RX interrupts, executes DI/EHB, accepts bounded host input `0x5A`, observes IP4 while masked, then enables through EI. The pending request enters the guest handler before the following EHB; the handler records Cause before reading RBR, acknowledges the FIFO read, returns by ERET to execute EHB, and verifies the byte and resumed marker. The pre-EI snapshot replays identically.
- **Isolation:** CPU0 DI does not change CPU1 Status.IE; an independent VM retains its own status and execution state.

## Authentic Linux continuation

The new bounded trace is `docs/artifacts/openwrt_malta_be64_di_ei_phase19.jsonl` (77,665 bytes; SHA-256 `159F4B8EBE94F3FA1DF82340D8F08812B03D5E9A0978812AEEA01175DC5484B3`). It records the first 256 attempts, the omitted span, architectural DI events, summary, and final frontier.

At the authentic DI boundary:

| State | Observed value |
|---|---|
| PC VA / fetch PA | `0xFFFFFFFF80B426C0` / `0x00B426C0` |
| Instruction | `0x41606000` (`DI $zero`) |
| Status before | `0x0000000000400080` (IE was already clear; IM=0) |
| Cause / pending IP | `0x0` / `0x0` |
| Count / Compare | `151572` / `0` |
| EPC / ErrorEPC / BadVAddr | all `0` |
| KSU / EXL / ERL | kernel / clear / clear |
| Hardware IRQ lines / UART RX | none / none |
| LL/SC reservation | invalid |

DI retired normally, returned the prior Status to `$zero`, preserved the observed Status value (IE was already clear), advanced PC to `0xFFFFFFFF80B426C4`, and advanced Count and VM cycles to 151,573. Executing DI once, restoring the captured full-VM snapshot, and executing it again produced identical CPU and VM state.

Continuation ended after 151,585 attempts and 151,584 total retired instructions. That is 151,567 retired instructions since Linux entry and 12 retired instructions beyond the former DI frontier (DI plus 11 following Linux instructions). It recorded one DI, no EI, three EHBs, no interrupt deliveries, zero UART bytes, no exception entry, and a valid final snapshot. The stop was a fetched valid-but-unsupported instruction at VA `0xFFFFFFFF8012F5A8`, PA `0x0012F5A8`, word `0x7C83C983`. Independent field decoding gives SPECIAL3/funct 3: `DEXT $v1,$a0,6,26` (rs=4, rt=3, msbd=25 => size 26, lsb/pos=6). The manual defines this DEXT encoding as MIPS64 Release 2 and the field bounds are valid (p. 139). The emulator reported `UnsupportedInstruction`, not a reserved instruction or architectural exception. No DEXT implementation was added in this phase.

The kernel's Status.IE was already clear at DI, so this authentic continuation validates the authentic boundary and retirement but does not demonstrate an IE 1-to-0 transition in Linux. The independent unit and production timer/UART guests exercise DI from IE=1 and EI from IE=0.

## Regression and production evidence

- `test_mips64_di_ei`: 681 checks passed, 0 failed.
- Direct `test_mips64_elf_handoff.exe`: 196 checks passed, 0 failed.
- MIPS64 CTest after the final runner build: 47/47 suites passed, including all 46 inherited suites.
- Selected cross-ISA CTest: `BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`, and `RiscVDecoderSmokeTest`: 6/6 passed.
- Production `ia64emu.exe` built successfully. Its symbol table contains `Mips64Cpu::step`, `opName`, `disassemble`, `serialize`, and `deserialize`, plus full-snapshot integrity methods. The linked image contains DI/EI, EHB, JR.HB, and JALR.HB names; the DI/EI focused CTest exercises exact decoding and disassembly.
- Previous DSRL32, SLTIU, LLD/SCD, TNE, SLLV, LL/SC, interrupt, UART, and snapshot suites all passed in the 47-suite run. The authenticated kernel SHA remained unchanged; earlier artifacts remain preserved.

## Known limits and MIPS64-20 recommendation

The core still lacks CU0-mode fidelity and full MIPS64r2 SPECIAL3 bitfield operations. The Linux continuation did not reach EI because Status.IE remained clear and the next frontier was DEXT. Linux did not boot fully; the bounded run stopped cleanly before executing DEXT.

MIPS64-20 should implement the DEXT family beginning with the exact `DEXT $v1,$a0,6,26` frontier, preserve valid-versus-reserved decoding distinctions, add encoded endianness and boundary tests, and continue the unchanged kernel. Extend to DEXTU/DEXTM only as required by architecture tests or the next authentic frontier; avoid unrelated SPECIAL3 work.
