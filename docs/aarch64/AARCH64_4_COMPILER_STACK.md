# AARCH64-4 — Compiler-Style Stack Frames, Memory Addressing, Logic and Select

## 1. Repository state and phase boundary

The required local checkout was `D:\dev\guideXOS_Hypervisor_AARCH64` on
branch `AARCH64`, at starting HEAD
`70365d1fb2c423046e7995c6db6f703bdbd1bb34`, tracking `origin/AARCH64`.
The root `.phase` belongs to IA-64 and was left untouched. At entry,
`docs/aarch64/.phase` said `last_completed=AARCH64-3` and
`next_expected=AARCH64-4`.

The worktree already contained uncommitted AARCH64-4 implementation work in
`CMakeLists.txt`, the AArch64 CPU/plugin headers, and the AArch64 CPU/plugin
sources, plus an untracked `tests/test_aarch64_compiler_stack.cpp`. The
untracked `build/` directory was also present. That work was reviewed and
completed in place; `build/` was preserved and excluded from the phase commit.
No other checkout or branch was used. The accepted foundation suite was run
before further edits: 221 checks, 0 failures. No AARCH64-4 acceptance document
or advanced phase marker existed.

Build environment recorded from the existing build tree: CMake 4.3.1, Ninja
1.13.2, MinGW-w64 GCC 15.2.0, Release configuration. No AArch64 assembler was
available on `PATH`; the field encodings were cross-checked against the A64
encoding tables and shared pseudocode below, and executed through the real
decoder. Expected values in the tests are fixed independently of the
production decoder.

## 2. Architectural references

The authority is Arm's [A64 Instruction Set Architecture for A-profile,
DDI0602](https://developer.arm.com/documentation/ddi0602/latest). The
published field tables and pseudocode used for instruction-level cross-checks
are available at:

- [STP](https://arm.jonpalmisc.com/latest_aarch64/stp_gen) and
  [LDP](https://arm.jonpalmisc.com/latest_aarch64/ldp_gen)
- [LDR immediate](https://arm.jonpalmisc.com/latest_aarch64/ldr_imm_gen),
  [STR immediate](https://arm.jonpalmisc.com/latest_aarch64/str_imm_gen),
  [LDUR](https://arm.jonpalmisc.com/latest_aarch64/ldur_gen), and
  [STUR](https://arm.jonpalmisc.com/latest_aarch64/stur_gen)
- [AND immediate](https://arm.jonpalmisc.com/latest_aarch64/and_log_imm),
  [ANDS immediate](https://arm.jonpalmisc.com/latest_aarch64/ands_log_imm),
  [CSEL](https://arm.jonpalmisc.com/latest_aarch64/csel), and
  [CSINC](https://arm.jonpalmisc.com/latest_aarch64/csinc)

The instruction-reference site is a secondary presentation of Arm's
machine-readable pseudocode. Where a presentation differs from DDI0602, the
Arm architecture specification takes precedence.

## 3. Implemented behavior

### STP/LDP integer register pairs

The decoder accepts the general-purpose 32-bit (`opc=00`) and 64-bit
(`opc=10`) pair forms and rejects the other pair opcode classes as unsupported
or unallocated. The imm7 field is sign-extended before scaling by four bytes
for W pairs or eight bytes for X pairs. Signed-offset, pre-index, and
post-index forms are supported. Rn=31 selects SP; Rt/Rt2=31 reads as zero or
discards a load. Little-endian transfers place Rt at the lower address and
Rt2 at the next element. W loads zero-extend into the X register view. Pair
operations preserve NZCV and unrelated registers.

Each pair uses one contiguous `IMemory::Read` or `IMemory::Write`. LDP stages
both values locally and commits registers and writeback only after the single
read succeeds. Writeback and PC do not commit on a failed transfer. The
interface cannot promise rollback for every arbitrary `IMemory::Write`: a
backend may mutate a prefix and then throw. The supported flat test memory
checks the full range before copying, so its out-of-bounds pair store changes
no bytes. A second test backend deliberately writes four bytes and throws;
the test confirms one transfer call, no CPU writeback, and no PC advance while
allowing those four backing bytes to remain changed.

### Signed and indexed single-register accesses

LDUR/STUR decode imm9 as a signed byte displacement in the range -256 to
+255, without scaling or writeback. LDR/STR pre-index uses base plus signed
imm9 for the transfer and writes the updated base after success. Post-index
uses the original base for the transfer and commits base plus imm9 afterward.
The existing unsigned-offset form remains scaled by access size. W and X
forms, SP bases, and zero-register destinations/sources are supported.

Ordinary unaligned Normal-memory transfers are accepted. Effective-address
addition wraps modulo 2^64. Before calling a backing memory object, the CPU
rejects a transfer whose byte range would cross the end of the 64-bit address
space; normal backing-memory bounds failures report `MemoryFault`. Faulting
loads preserve the destination, base and PC. Faulting stores do not update a
writeback base or PC. Standard and non-standard memory exceptions are
contained as architectural faults.

### Logical immediate operations

AND, ORR, EOR, and ANDS use the ARM `DecodeBitMasks` process: derive the
element size from N and the inverted imms separator, reject reserved
all-ones/invalid elements, generate a run of ones, rotate it by immr, then
replicate the element to the operation width. Both W and X forms are
implemented. The test vectors cover repeated 8/16/32-bit patterns, rotated
masks, the high W bit, and invalid encodings.

For AND/ORR/EOR, Rn=31 reads WZR/XZR and Rd=31 writes WSP/SP. These forms
preserve NZCV. For ANDS, Rd=31 is WZR/XZR and implements TST; other
destinations receive the result. ANDS sets N and Z from the width-masked
result and clears C and V. Disassembly uses `tst` for the ANDS alias and
`mov` for ORR with Rn=31.

### Conditional selection

CSEL and CSINC support W and X operands. CSINC increments the second operand
only when the condition is false; unsigned addition provides defined X
wraparound and explicit W masking provides 32-bit wraparound. Register 31 is
WZR/XZR for all operands and destinations. Both operations preserve NZCV.
Condition evaluation was checked for all 16 condition codes against all 16
NZCV combinations, including AL and NV, which both evaluate true in the Arm
`ConditionHolds` pseudocode. Disassembly recognizes CSET and CINC when their
encoding conditions satisfy the alias rules.

## 4. Conservative encoding and memory-fault policy

The following constrained-unpredictable forms return `UnsupportedFeature`
without changing architectural state:

- LDP with Rt equal to Rt2, in any addressing mode.
- LDP writeback with Rn equal to Rt or Rt2.
- STP writeback with Rn equal to either source register.
- Writeback LDR/STR where the base and transfer register overlap.

The architectural base-register comparison excludes Rn=31, whose base is SP
and whose transfer register 31 is WZR/XZR. STP with identical source
registers remains accepted. LDUR/STUR have no writeback and do not use the
writeback-overlap restriction. Unprivileged LDTR/STTR encodings are
recognized as outside this profile rather than being mislabeled unallocated.
LDPSW and MTE STGP are outside the implemented subset.

No new fault category was needed. Invalid encodings retain deterministic
`UnallocatedEncoding` behavior; allocated but unsupported forms use
`UnsupportedFeature`; backing-memory and address-range failures use
`MemoryFault`. Instruction fetch alignment and the existing fault taxonomy
remain in force. This bounded profile does not add SP-alignment checks, an
MMU, Device memory, exception levels, or SCTLR-controlled alignment behavior.

## 5. Integrated Program A — three-level stack-frame call chain

The real A64 code starts at `0x1000`. Main initializes SP to `0x9000`, sets
result/data pointers, and BLs to A at `0x1020`. A, B, and C each use the
compiler-style prologue `STP X29,X30,[SP,#-16]!`, establish X29 with
`ADD X29,SP,#0`, and restore the pair with `LDP X29,X30,[SP],#16` before
RET. C sets X0 to 40, stores and reloads it through guest address `0x2010`,
then adds two. B adds three and A adds five. Main stores the result at
`0x2000` and branches to the completion PC `0x101C`.

| Frame | SP / X29 while active | Saved X29 | Saved X30 | Return address |
|---|---:|---:|---:|---:|
| A | `0x8FF0` | original `0xAABBCCDDEEFF0011` | `0x1014` | main `0x1014` |
| B | `0x8FE0` | `0x8FF0` | `0x102C` | A `0x102C` |
| C | `0x8FD0` | `0x8FE0` | `0x104C` | B `0x104C` |

All BL boundaries have a 16-byte-aligned SP: `0x9000`, `0x8FF0`, and
`0x8FE0`. The frames occupy separate 16-byte slots. Execution takes exactly
27 instructions and ends at PC `0x101C`, SP `0x9000`, X29
`0xAABBCCDDEEFF0011`, and X30 `0x1014`. The return sequence is C to
`0x104C`, B to `0x102C`, and A to `0x1014`. X0 and `[0x2000]` are 50;
`[0x2010]` is 40. Canary ranges `[0x8F00,0x8FD0)` and
`[0x9000,0x9100)` are intact. Two executions from identical initial states
produce identical CPU state and complete guest-memory images. No guest
register is repaired between instructions by the test harness.

## 6. Integrated Program B — logical and conditional execution

The program constructs `X0=0xDEF09ABC56781234` with MOVZ/MOVK, then ANDs to
`X1=0x34`, ORs to `X2=0x3F`, and XORs to `X3=0`. `TST X2,#0x40` sets Z, so
the EQ CSEL selects `0x111`, the NE CSEL selects `0x222`, the EQ CSINC takes
its first operand (`0x333`), and the false NE CSINC increments
`UINT64_MAX` to zero. CMP on `UINT64_MAX` against one sets N and C: signed LT
is true and unsigned LO is false. A second 32-bit sequence constructs
W20=`0x8000FFFF`; TST of bit 31 sets N and drives both MI and PL CSEL paths.
The program also takes and skips B.cond paths and stores W/X results through
the normal guest-memory instructions.

It executes exactly 49 instructions and ends at its documented self-loop PC
`0x10CC`, with SP `0`, PSTATE NZCV `0x80000000` (`N=1,Z=0,C=0,V=0`), and outputs:

| Guest address | Width | Expected value |
|---|---:|---:|
| `0x3000` | X | `0x111` |
| `0x3008` | X | `0x222` |
| `0x3010` | X | `0` |
| `0x3018` | W | `0x222` |
| `0x3020` | X | `0x111` |
| `0x3028` | X | `0x123` |
| `0x3030` | W | `0xABCD` |
| `0x3034` | W | `0x1234` |

The test also verifies X0/X1/X2/X3 and all selected results directly, confirms
the W calculation is zero-extended, and repeats execution from a clean state
with identical outputs.

## 7. Integrated Program C — memory addressing matrix

The bounded eight-instruction program independently checks each effective
address and writeback point:

| Step | Instruction | Transfer address | Base after success |
|---:|---|---:|---:|
| 1 | `STUR X0,[X1,#-3]`, X1=`0x4000` | `0x3FFD` | `0x4000` |
| 2 | `LDUR X2,[X1,#-3]` | `0x3FFD` | `0x4000` |
| 3 | `STR X0,[X1,#5]!` | `0x4005` | `0x4005` |
| 4 | `LDR X3,[X1],#-2` | `0x4005` | `0x4003` |
| 5 | `STP X4,X5,[X1,#-16]` | `0x3FF3` | `0x4003` |
| 6 | `LDP X6,X7,[X1,#-16]` | `0x3FF3` | `0x4003` |
| 7 | `STP X4,X5,[X1,#16]!` | `0x4013` | `0x4013` |
| 8 | `LDP X6,X7,[X1],#-16` | `0x4013` | `0x4003` |

The final PC is `0x20`; X2/X3 equal `0x0102030405060708`, X6 is
`0x1111222233334444`, and X7 is `0xAAAABBBBCCCCDDDD`. Protected accesses at
and beyond the memory limit fault without register/PC/writeback changes.
Canaries around the transfer regions and in untouched gaps remain unchanged.

## 8. Plugin, state format, and validation results

All new operations are available through the production AArch64 decoder,
executor, `step()`, disassembler, and plugin feature reporting. The new test
also runs pair, unscaled, indexed, logical, CSEL, and CSINC instructions
through `AArch64ISAPlugin::step`, checks aliases and independent plugin
instances, and inspects state after execution. Serialization remains the
existing 280-byte version-1 format (`A641`, version 1, X0-X30, SP, PC, PSTATE,
zero reserved word). A hand-built v1 blob restores correctly; bad magic,
length, and nonzero reserved word are rejected without changing live state.

Exact build and regression commands:

```powershell
cmake --build build --target test_aarch64_foundation test_aarch64_memory_branch test_aarch64_pc_stack test_aarch64_compiler_stack test_efi_memory_map test_ia64_sal test_riscv_decoder
ctest --test-dir build -R '^(AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest)$' --output-on-failure
git diff --check
```

Results from the test executables: Foundation 221 checks / 0 failures;
MemoryBranch 1,304 / 0; PcStack 940 / 0; CompilerStack 5,221 / 0. CTest
passed 7/7 suites. EfiMemoryMapTests, IA64SalTests, and RiscVDecoderSmokeTest
passed. `git diff --check` passed. The IA-64 `test_isa_plugin` failure
documented at `tests/test_isa_plugin.cpp:1551` is outside these gates and was
not changed or rerun; no IA-64 or RISC-V source was modified.

## 9. Supported-profile limits and AARCH64-5 recommendation

This phase does not implement LDPSW/STGP, LDTR/STTR execution, STNP/LDNP,
SIMD/FP pairs, SIMD/FP, EL state, MMU translation, Device memory, atomics,
SMP, or a full AAPCS64 environment. Unaligned transfers are accepted under
the existing flat Normal-memory profile; no SCTLR-dependent alignment or SP
alignment fault is modeled.

Recommended AARCH64-5 scope: add the common general-purpose shifted-register
ADD/SUB/AND/ORR/EOR families and byte/halfword load/store forms, with encoding
collisions and sign/zero extension independently tested. Use small
compiler-produced leaf functions to demonstrate each new dependency, while
keeping MMU and platform-device work out of the ISA phase.
