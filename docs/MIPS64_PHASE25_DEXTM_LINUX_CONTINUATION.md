# MIPS64-25 — DEXTM Qualification and OpenWrt Linux Continuation

**Outcome A — complete.** The authenticated DEXTM instruction was reproduced at the Phase 24 frontier, decoded as SPECIAL3 function `0x01`, and retired by the production Malta VM with an independently calculated 62-bit extraction result. Linux continued to the next unsupported instruction. Full Linux boot is not claimed.

## 1. Repository preflight and baseline

- Checkout: `D:\dev\guideXOS_Hypervisor_MIPS64`; branch: `MIPS64`.
- Starting HEAD: `6632ef710917873af9bf8d23a5c4d3d56782b6e9`, the expected MIPS64-24 commit.
- Starting worktree: clean. Local/upstream divergence was 3 ahead and 0 behind `origin/MIPS64`.
- `.phase-mips64` recorded MIPS64-24 complete and MIPS64-25 next. The separate IA-64 `.phase` marker was read and left unchanged.
- The MIPS64-20 and MIPS64-24 reports, accepted Phase 24 trace, DEXT decoder/executor/disassembler, neighboring SPECIAL3 classification, encoded tests, and snapshot replay paths were reviewed.
- `E:\CodexArtifacts\guideXOS_Hypervisor` and documented repository artifact locations were inspected. The existing ignored kernel under `build/mips64-12-artifacts` was reused; no guest image or accepted trace was regenerated.
- All **52 inherited MIPS64 CTest suites passed before implementation**. The accepted Phase 24 aggregate was 421,554 assertions.

No branch, worktree, or Git topology operation was performed. Nothing was pushed.

## 2. Kernel identity and Malta handoff

| Property | Value |
|---|---|
| Distribution / platform | OpenWrt 24.10.4 / Malta BE64 |
| Linux version | 6.6.110 |
| ELF | `build/mips64-12-artifacts/openwrt-24.10.4-malta-be64-vmlinux.elf` |
| Size | 11,013,776 bytes |
| SHA-256 | `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc` |
| Entry | `0xFFFFFFFF80917670` |
| RAM / endianness | 256 MiB / big-endian |
| PROM command line | `console=ttyS0,38400n8` |
| Malta ROM | Existing 4 MiB reset/PROM image |

The ELF hash was checked before and after replay. Its bytes were not changed. The ROM/PROM handoff was configured through the production Malta `VirtualMachine` with the prior accepted configuration.

## 3. Phase 24 frontier authentication

The accepted Phase 24 trace contains 2,051 JSONL records and remains SHA-256 `af83f13c4b9e8593d1f757a8a656a2ea987b21ab21c50677255b9092c0613037`. Before editing the decoder or executor, the existing production VM replayed the unchanged kernel and stopped at the same unsupported word. The pre-change trace in the ignored build directory is byte-for-byte identical to the accepted Phase 24 trace hash.

| Observation | Reproduced value |
|---|---|
| Retired / Count / VM cycles before DEXTM | 153,888 / 153,888 / 153,888 |
| PC VA / fetch PA | `0xFFFFFFFF80188804` / `0x000188804` |
| Guest memory word | `0x7E31E801` |
| Status / Cause / EPC / BadVAddr | `0x00400080 / 0 / 0 / 0` |
| Delay slot | Clear |
| Architectural exception | None |
| UART TX / RX / console bytes | 0 / 0 / 0 |

The accepted Phase 24 trace's `source_value` is correct. Its `destination_before` field was populated using the encoded `rd` field (29, `$sp`) rather than architectural destination `rt`. This phase independently captures the actual DEXTM destination prestate from `$s1`.

## 4. DEXTM encoding and semantics

The fetched word decodes as:

| Field | Value |
|---|---:|
| Primary opcode | `0x1F` — SPECIAL3 |
| `rs` | 17 — `$s1` |
| `rt` | 17 — `$s1`, destination |
| Bits 15:11 | 29 — `msbdminus32` |
| Bits 10:6 | 0 — position / `lsb` |
| Function | `0x01` — DEXTM |
| Effective `msbd` / size | 61 / 62 bits |
| Disassembly | `dextm $17, $17, 0, 62` |

The [MIPS64 Architecture for Programmers, Volume II-A, Revision 5.04, DEXTM p. 141](https://s3-eu-west-1.amazonaws.com/downloads-mips/documents/MD00087-2B-MIPS64BIS-AFP-05.04.pdf) defines `msbd = msbdminus32 + 32`, making the extracted width `msbd + 1`, or encoded field plus 33. DEXTM extracts source bits 61 through 0, right-justifies them, and zero-extends the result. The independent [GNU Binutils MIPS opcode definitions](https://gnu.googlesource.com/binutils-gdb/+/refs/heads/master/include/opcode/mips.h) also distinguish the DEXTM encoded size field.

DEXT, DEXTM, and DEXTU remain distinct: functions `0x03`, `0x01`, and `0x02` respectively. The DEXT field remains `MSBD = size - 1`; DEXTM uses `size = encoded field + 33`. DEXTU, DINS, and other SPECIAL3 operations remain unsupported.

For the live Linux prestate:

```text
source $s1       = 0x00000000FFFFF000
destination $s1  = 0x00000000FFFFF000
pos               = 0
size              = 29 + 33 = 62
expected result   = 0x00000000FFFFF000
```

The expected result was independently computed bit-by-bit. Source is read before destination write, so `$rs == $rt` aliasing is well-defined. The result matches the production VM's observed `$s1` after retirement.

## 5. Defined and UNPREDICTABLE field handling

The decoder accepts defined fields with `0 <= pos < 32`, `33 <= size <= 64`, and `pos + size <= 64`. DEXTM field combinations that exceed bit 63 are architecturally UNPREDICTABLE per the MIPS64 manual. They are rejected with the deterministic `UnsupportedInstruction` stop policy and an explicit UNPREDICTABLE diagnostic; no Reserved Instruction exception is entered and no GPR or memory is changed.

Execution validates the range before reading operands. The 64-bit mask uses `UINT64_MAX`; narrower fields use `(uint64_t{1} << size) - 1`. This avoids evaluating a shift by 64. Execution uses an unsigned logical right shift, writes `rt`, preserves `$zero`, and has no memory, CP0, HI/LO, TLB, or reservation side effects.

## 6. Implementation and focused architectural tests

The implementation is in `include/mips64/Mips64Cpu.h` and `src/mips64/Mips64Cpu.cpp`; the encoded tests are in `tests/test_mips64_dextm.cpp`, the existing DEXT neighbor classification was updated in `tests/test_mips64_dext.cpp`, and CMake registers `Mips64DextmTests`.

- Appended `Mips64Op::Dextm` to preserve existing enum values.
- Decoded SPECIAL3 function `0x01` only; DEXT function `0x03` remains unchanged and DEXTU function `0x02` remains unsupported.
- Reused decoded `rd` as `msbdminus32` and `sa` as position, with dedicated comments and DEXTM disassembly/operation naming.
- Preserved other SPECIAL3 allocated-versus-reserved classifications.
- Retired the instruction through the existing CPU path; no persistent architectural state or snapshot format changed.

The focused DEXTM target passed **2,609 checks**. Encoded coverage includes the exact Linux word and field decoding; widths 33, 34, 48, 62, 63, and 64; positions 0, 1, 2, and 31; `pos + size == 64` and `> 64`; zero/all-ones and individual high/low bits; mixed operands; logical shift and zero extension; full-width mask; aliasing; source/destination `$zero`; both endian fetch modes; PC/Count/retirement; taken and not-taken delay slots; GPR/CP0/HI/LO/memory preservation; LL reservation preservation; DEXT regression and DEXTU/DINS/reserved SPECIAL3 classification; disassembly; CPU serialization; and snapshot replay.

The existing DEXT suite passes **4,357 checks** and now explicitly expects the authenticated DEXTM word to decode as DEXTM while keeping neighboring operations distinct.

## 7. Snapshot and deterministic replay

Synthetic tests capture immediately before encoded DEXTM, execute, restore, and repeat. A branch-delay test captures the VM with DEXTM armed in the slot, replays it for both taken and not-taken branches, and matches the destination, target PC, delay state, and counters. A corrupted snapshot is rejected without changing live state or advancing virtual time.

The authentic Linux full-VM snapshot was captured immediately before the real DEXTM. The instruction and continuation were executed, the snapshot was restored, and the same DEXTM plus continuation was repeated. Replay matched CPU state, full RAM and reservation generations, device state, UART, console-visible output, scheduler state, cycle and retirement counts, and stop reason.

Accepted formats remain unchanged: full VM `GXM64SNP` v3, CPU v6 (1,001 bytes), memory v2, UART v4, and IRQ routing v1. DEXTM adds no persistent state.

## 8. Authentic Linux retirement and next frontier

The production Malta VM fetched the instruction from guest memory and retired it normally:

| Observation | Result |
|---|---|
| `$s1` before / expected / observed | `0x00000000FFFFF000` / same / same |
| PC before / after | `0xFFFFFFFF80188804` / `0xFFFFFFFF80188808` |
| Delay slot before / after | Clear / clear |
| Count / VM cycles / retired after DEXTM | 153,889 / 153,889 / 153,889 |
| Cause / EPC / BadVAddr | `0 / 0 / 0` |

The unchanged kernel then retired **3,891 further instructions** and stopped at the next valid-but-unsupported SPECIAL instruction:

| Observation | Value |
|---|---|
| Next PC VA / fetch PA | `0xFFFFFFFF8091EB50` / `0x00091EB50` |
| Raw word | `0x0000000F` |
| Decode status | SPECIAL function `0x0F`, valid encoding outside the implemented subset |
| Stop reason | Unsupported instruction; the frontier was not executed |
| Total retired / attempted | 157,780 / 157,781 |
| Retired since Linux entry | 157,763 |
| Retired beyond the former DEXTM frontier | 3,892, including DEXTM |
| Distinct retired PCs after DEXTM | 796 |
| Count / VM cycles | 157,780 / 157,780 |
| Status / Cause / EPC / BadVAddr | `0x00400080 / 0 / 0 / 0` |
| Valid TLB entries | 0 |
| UART TX / RX / console output | 0 / 0 / 0 bytes |

The stop was at a decoded unsupported instruction before the one-million-instruction bound. No architectural exception was entered. MMIO access counts were not collected. Linux boot completion remains unverified.

## 9. Regression, production linkage, and trace integrity

- Pre-change inherited MIPS64 baseline: **52/52 passed**.
- Final MIPS64 CTest: **53/53 passed**.
- Total reported MIPS64 assertions: **424,163** (421,554 inherited plus 2,609 DEXTM checks).
- Selected IA-64/RISC-V CTest regressions: **6/6 passed** — `BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`, and `RiscVDecoderSmokeTest`.
- Rebuilt all 37 MIPS64 test executable targets and production `ia64emu.exe`. The production image contains linked `decodeInstruction`, `Mips64Cpu::executeDecoded`, `Mips64Cpu::disassemble`, and `Mips64Cpu::opName` symbols, plus the `dextm` mnemonic and UNPREDICTABLE diagnostic.
- The authenticated Phase 24 trace, the pre-implementation replay, and the final replay all reproduce the same 256-record initial prefix. The final run also compared all 1,787 overlapping Phase 24 continuation instruction records byte-for-byte through the DEXTM frontier. No identity claim is made for records outside those overlaps.
- Historical Phase 12–24 reports and traces remain unchanged. Phase 24 trace SHA-256 remains `af83f13c4b9e8593d1f757a8a656a2ea987b21ab21c50677255b9092c0613037`; kernel SHA-256 remains the value in Section 2.

The new bounded trace [`docs/artifacts/openwrt_malta_be64_dextm_phase25.jsonl`](artifacts/openwrt_malta_be64_dextm_phase25.jsonl) contains 5,941 valid JSONL records. It records the production prestate, retirement, snapshot replay, overlapping Phase 24 instructions, continuation, and next frontier. SHA-256: `222593af3cf8e35b2b415190daf3bf1a0b9c00d90eebb03b8d86f6e8752c0c2f`.

## 10. Limits and MIPS64-26 recommendation

DEXTU, DINS-family instructions, arbitrary SPECIAL3 operations, and full Linux boot remain out of scope. The next frontier is word `0x0000000F`, the SPECIAL function encoding for SYNC. MIPS64-26 should authenticate its field encoding and shared-memory ordering requirements, then qualify only the behavior needed by this Malta VM. In particular, do not assume SYNC is a no-op until the emulator's memory, device, and ordering model is checked against the [MIPS64 SYNC definition and SType table](https://s3-eu-west-1.amazonaws.com/downloads-mips/documents/MD00087-2B-MIPS64BIS-AFP-05.04.pdf).

Previously known unrelated limitations remain: the IA-64 NaT assertion at `tests/test_isa_plugin.cpp:1551`, six unavailable legacy CTest executables, and historical aggregate-build limitations. This phase did not change IA-64 source or those test targets.

No IA-64 source or `.phase` marker was changed. No guest binary, ROM/PROM configuration, accepted trace, or unrelated artifact was overwritten. The Phase 25 implementation and evidence are committed locally on `MIPS64`; no push was made.
