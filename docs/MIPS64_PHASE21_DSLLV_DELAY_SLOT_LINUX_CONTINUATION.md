# MIPS64-21 — DSLLV Delay-Slot Qualification and OpenWrt Continuation

**Outcome A — complete.** The authentic OpenWrt DSLLV instruction was independently decoded, implemented, retired by the production Malta VM, and replayed from an armed-delay-slot full-VM snapshot. Its pending not-taken branch completed at the correct fall-through PC. The unchanged kernel retired five more instructions before the next valid but unsupported instruction. Linux boot completion is not claimed.

## 1. Repository preflight

- Checkout: `D:\dev\guideXOS_Hypervisor_MIPS64`.
- Branch: `MIPS64`.
- Starting HEAD: `535a34ff8f551c9382572e4f3f8184d433e045c8`, the expected MIPS64-20 commit.
- Initial worktree: clean; upstream divergence: 1 ahead, 0 behind `origin/MIPS64`.
- `.phase-mips64` recorded MIPS64-20 complete and MIPS64-21 next. The IA-64 `.phase` marker was left unchanged.
- MIPS64-13 through MIPS64-20 reports and CPU/branch-delay findings were reviewed. The Phase 20 bounded trace was inspected.
- All 48 inherited MIPS64 CTest suites passed before implementation.
- `E:\CodexArtifacts\guideXOS_Hypervisor` was inspected. Its available phase/build directories included PPC64 and RISC-V artifacts, but no MIPS64 Malta kernel image. The accepted image in the local ignored `build/mips64-12-artifacts` directory was reused.

No branch, worktree, reset, rebase, merge, stash, clean, cherry-pick, push, or other topology operation was used. Existing accepted kernel, ROM/PROM setup, reports, traces, and user artifacts were preserved.

## 2. Authenticated kernel and Malta configuration

The exact accepted artifact was reused:

| Property | Value |
|---|---|
| ELF | `build/mips64-12-artifacts/openwrt-24.10.4-malta-be64-vmlinux.elf` |
| Distribution / target | OpenWrt 24.10.4 / Malta BE64 |
| Linux version | 6.6.110 |
| Size | 11,013,776 bytes |
| SHA-256 | `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc` |
| Entry | `0xFFFFFFFF80917670` |
| Guest RAM | 256 MiB, big-endian |
| PROM command line | `console=ttyS0,38400n8` |

The existing production `VirtualMachine`, Malta ROM/PROM handoff, and RAM configuration were retained. The ELF hash was rechecked after the run. The guest image was neither regenerated nor patched.

## 3. Phase 20 acceptance baseline

The Phase 20 trace recorded 151,592 retired instructions and 151,593 attempts, stopping before execution at PC `0xFFFFFFFF8012F5C8`, word `0x00832014`. Its prior DEXT was at `0xFFFFFFFF8012F5A8`, and the DSLLV slot was armed. UART output and architectural exception state were empty/clear; Linux boot was unverified.

The production VM reproduced the prefix and the exact DSLLV prestate. The first 256 instruction records in the new trace match the accepted Phase 20 records byte-for-byte (zero mismatches). Only those overlapping records are claimed identical.

## 4. DSLLV encoding and architecture

The big-endian bytes `00 83 20 14` form raw word **`0x00832014`**:

| Field | Value |
|---|---:|
| Primary opcode | `SPECIAL` (`0x00`) |
| `rs` | 4 (`$a0`, shift count) |
| `rt` | 3 (`$v1`, value source) |
| `rd` | 4 (`$a0`, destination) |
| `sa` | 0 (reserved field) |
| Function | `0x14` |
| Operation | `DSLLV $a0,$v1,$a0` |

MIPS64 Release 2 shifts the full 64-bit `GPR[rt]` left by `GPR[rs][5:0]`, fills with zeros, and discards bits shifted beyond bit 63. The result has no 32-bit truncation or sign extension. The [MIPS64 Architecture for Programmers, Volume II, Revision 2.50, DSLLV entry](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf) specifies the encoding, six-bit shift count, and logical result.

## 5. Authentic branch and DSLLV prestate

The instruction immediately before DSLLV is the branch at `0xFFFFFFFF8012F5C4`:

```text
0xffffffff8012f5c0  0x3086003f  andi $a2,$a2,0x3f
0xffffffff8012f5c4  0x10a0000c  beq  $a1,$zero,12
0xffffffff8012f5c8  0x00832014  dsllv $a0,$v1,$a0   # delay slot
```

The branch operands were observed as `$a1=1` and `$zero=0`, so the branch is **not taken**. Its target if taken is `0xFFFFFFFF8012F5F8`; its not-taken post-delay PC is `0xFFFFFFFF8012F5CC`. Before executing the branch, Count and VM cycles were 151,591. Executing the branch armed the slot and advanced to the DSLLV PC without changing the pending target.

Immediately before DSLLV, the live VM showed:

| Observation | Value |
|---|---:|
| Retired / attempted boundary | 151,592 / 151,592 |
| `$a0` (both shift register and destination) | `0x0000000000000000` |
| `$v1` (value source) | `0x0000000000000001` |
| Effective count, `$a0 & 0x3F` | 0 |
| Independently calculated result | `0x0000000000000001 << 0 = 0x0000000000000001` |
| Delay armed / originating branch | yes / `0xFFFFFFFF8012F5C4` |
| Delay target | `0xFFFFFFFF8012F5CC` |
| Status / Cause / EPC / BadVAddr | `0x00400080 / 0 / 0 / 0` |
| Count / VM cycles | 151,592 / 151,592 |
| HI / LO / LL reservation / valid TLB entries | `0 / 0 / inactive / 0` |
| Preinstruction full-VM snapshot | valid `GXM64SNP` v3 |

## 6. Minimal CPU implementation

`Mips64Op::Dsllv` was appended to preserve prior enum values. The SPECIAL decoder recognizes function `0x14` and rejects nonzero `sa` as a reserved instruction. Execution reads the value and shift-count registers before writing the destination, masks the shift amount to six bits, performs an unsigned `uint64_t` left shift, and uses the existing GPR write helper. The normal retirement tail resolves the armed branch and clears `delayArmed`, `delayBranch`, and `delayTarget`; DSLLV has no special PC path. Disassembly and `opName` report `dsllv`.

The operation mutates no GPR other than `rd` and does not modify CP0, HI/LO, TLB, RAM, or the LL/SC reservation. CP0 Count and VM cycles each advance once through normal instruction execution. No DSRLV/DSRAV implementation or broad SPECIAL acceptance was added.

## 7. Focused encoded-instruction qualification

The new `test_mips64_dsllv` target executes encoded guest instructions and passed **5,213 checks**. It covers the exact Linux word and fields; `sa` rejection; shifts 0, 1, 7, 15, 31, 32, 47, and 63; shift-register values 64, 65, and 127; ignored upper count bits; zero, all-one, highest-bit, lowest-bit, mixed, and high-bit-discard cases; full-width results without word truncation/sign extension; and every required source/destination alias and `$zero` case.

Both little- and big-endian instruction fetch are covered. The suite checks normal retirement, PC/Count and VM cycles, disassembly, unrelated GPR/CP0/HI/LO/TLB and memory preservation, reservation preservation, and malformed encoding rejection without partial GPR writes. Independent examples include:

```text
0x0000000000000001 << 32 = 0x0000000100000000
0x0000000000000001 << 63 = 0x8000000000000000
0xffffffffffffffff << 1 = 0xfffffffffffffffe
0x1234567880000001 << 0 = 0x1234567880000001
```

The previously accepted DEXT suite was updated to assert that the now-implemented authenticated DSLLV decodes and disassembles correctly. DSRLV and DSRAV remain unsupported.

## 8. Branch-delay and exception regressions

Encoded tests cover taken and not-taken branches in both endian modes. They verify that the branch arms the slot, DSLLV updates its destination, the next PC equals the architectural branch target or fall-through, and delay state clears only after slot retirement.

A malformed DSLLV encoding faults in a delay slot. The tests verify RI (`Cause.ExcCode=10`), `Cause.BD=1`, EPC set to the originating branch, exception-vector entry, cleared pending branch state, and unchanged GPRs. Restoring the armed CPU and full-VM snapshots reproduces the same exception result. This exercises the existing exception-entry path without adding another delay mechanism.

## 9. Snapshot versions and replay scenarios

No snapshot format changed. The full-VM snapshot remains `GXM64SNP` v3, CPU serialization v6 (1,001 bytes), memory v2, UART v4, and IRQ routing v1.

Synthetic and authentic coverage includes: normal DSLLV snapshots; snapshots with a branch armed immediately before DSLLV, for both taken and not-taken conditions; snapshots from before the branch; a faulting delay-slot snapshot; and authentic Linux capture at `0xFFFFFFFF8012F5C8`. The authentic replay repeats DSLLV and the bounded continuation, matching CPU registers/PC/delay state/Count, VM cycles, memory and reservation state, UART, console guest-visible bytes/sequence, scheduler counters, and final frontier/stop reason. Console generation is an internal monotonic restore epoch and is not treated as guest-visible replay state.

An incompatible full snapshot was corrupted and rejected. The test verifies rejection leaves CPU, VM cycles/state, memory, devices, console and scheduler unchanged; restore does not execute an instruction or advance virtual time.

## 10. Authentic production-VM retirement

The production Malta VM reproduced the authenticated Phase 20 DEXT at `0xFFFFFFFF8012F5A8`. The observed DEXT source `$a0` and old `$v1` were both zero, so DEXT retired with independent result zero and was replayed from a full-VM snapshot. The unchanged execution then reached the same armed branch and DSLLV prestate described above.

The production VM retired DSLLV normally:

| Observation | Before | After DSLLV |
|---|---:|---:|
| `$a0` | `0x0000000000000000` | `0x0000000000000001` |
| `$v1` | `0x0000000000000001` | unchanged |
| PC | `0xFFFFFFFF8012F5C8` | `0xFFFFFFFF8012F5CC` |
| Pending branch | not-taken BEQ delay | completed |
| Delay state | armed, target `0xFFFFFFFF8012F5CC` | cleared |
| Count / VM cycles / retired | 151,592 | 151,593 |
| Status / Cause / EPC | `0x00400080 / 0 / 0` | unchanged |

The full-VM snapshot was restored at the armed slot, DSLLV was reexecuted, and the branch transition and subsequent Linux continuation matched. No register was changed by the host, and no guest instruction or branch was skipped.

## 11. Bounded Linux continuation and next frontier

The deterministic continuation budget allowed up to one million additional steps. The kernel stopped earlier at a genuine valid-but-unsupported SPECIAL instruction:

| Observation | Value |
|---|---|
| Total retired from reset | 151,598 |
| Retired since Linux ELF entry (17 ROM instructions preceded entry) | 151,581 |
| Retired beyond the former DSLLV frontier | 6, including DSLLV |
| Further instructions retired after DSLLV | 5 |
| Attempt boundary including next-frontier classification | 151,599 |
| Next PC / physical fetch | `0xFFFFFFFF8012F5E0` / `0x0012F5E0` |
| Raw word / fields | `0x00C31016`; SPECIAL, `rs=6`, `rt=3`, `rd=2`, `sa=0`, `funct=0x16` |
| Independent decode | `DSRLV $v0,$v1,$a2` |
| Stop reason | valid MIPS64 SPECIAL encoding outside the implemented subset |
| Final delay state | clear |
| CP0 Count / VM cycles | 151,598 / 151,598 |
| Status / Cause / EPC / BadVAddr | `0x00400080 / 0 / 0 / 0` |
| Valid TLB entries | 0 |
| UART transmitted / receive FIFO bytes | 0 / 0 |

The bounded runner classifies the next unsupported word before asking the CPU to execute it. Therefore the reported total attempts include one frontier classification beyond retired instructions, while Count and VM cycles remain at the last retired boundary. The fetch is from guest RAM; no architectural exception, bus/MMIO fault, or VM execution failure occurred. No detailed per-access MMIO counter is available in this run.

## 12. Trace and replay integrity

The new bounded trace is `docs/artifacts/openwrt_malta_be64_dsllv_phase21.jsonl`: **264 valid JSONL records**, 79,044 bytes, SHA-256 `5dd2aed393017e0f1b2d41aa7b1f834cd29643f9f45fd1a5fa142c1719681602`.

It contains the same 256 initial records as Phase 20, bounded gap records, the authenticated DEXT prestate/retirement, the exact branch and DSLLV prestate, DSLLV retirement, replay/continuation summary, and next frontier. The Phase 20 and earlier Phase 12–19 evidence remains preserved. The kernel SHA-256 was unchanged.

## 13. Regression totals

- MIPS64 CTest: **49/49 suites passed** (48 inherited plus DSLLV).
- MIPS64 assertion checks: **26,582 passed**. This combines the accepted 21,367-check Phase 20 baseline with the 5,213 DSLLV checks and the two additional DEXT checks that now qualify the implemented DSLLV frontier.
- Focused DSLLV: **5,213 passed**.
- Focused DEXT after updating its neighboring-instruction expectation: **4,357 passed**.
- Selected cross-ISA checks: **6/6 passed** — `BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`, and `RiscVDecoderSmokeTest`.
- All 33 MIPS64 test executable targets and production `ia64emu.exe` built successfully.

The known historical IA-64 NaT assertion and unavailable legacy CTest executables remain unrelated. No IA-64 source or marker was modified.

## 14. Production linkage

The production `build/bin/ia64emu.exe` symbol table contains `Mips64Cpu::executeDecoded`, `step`, `disassemble`, and `opName`; its strings include `dsllv`. The focused DSLLV test executable also linked the production MIPS64 sources and successfully ran the authenticated Linux continuation through the production `VirtualMachine`.

## 15. Phase tracking and commit

The phase marker is advanced to `last_completed=MIPS64-21` and `next_expected=MIPS64-22`. The IA-64 `.phase` marker remains unchanged. The final local commit and ending upstream divergence are recorded in the final report.

## 16. Remaining limitations

Linux boot, console initialization, and userspace remain unverified. The next stop is DSRLV at `0xFFFFFFFF8012F5E0`. No guest image, firmware, chipset behavior, snapshot format, or unrelated architecture code was changed.

## 17. Recommended MIPS64-22 scope

Independently qualify the exact authenticated DSRLV word and its observed operands at `0xFFFFFFFF8012F5E0`, implement only the demonstrated MIPS64 logical doubleword right shift, add encoded endianness/alias/delay/snapshot tests, and replay this same hashed kernel through the production VM. Keep DSRAV and other adjacent operations unsupported unless the authenticated stream requires them.
