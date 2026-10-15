# AARCH64-3 — PC-Relative Addressing, Literal Loads & Nested Stack-Based Subroutines

## 1. Starting repository state

- Local checkout: `D:\dev\guideXOS_Hypervisor_AARCH64` (single worktree, no other checkouts touched).
- Branch: `AARCH64` (verified with `git branch --show-current`; never switched/renamed).
- Starting HEAD: `18de6ee14f158f2cbb1d3938e53630c3fe02d09d` (clean tree, `git status` clean).
- Upstream: `origin/AARCH64`, in sync at start.
- `docs/aarch64/.phase` at start: `last_completed=AARCH64-2`, `next_expected=AARCH64-3`.
- Root `.phase` (IA-64 development) was never modified.
- Baseline before editing: `test_aarch64_foundation` 221 checks / 0 failures,
  `test_aarch64_memory_branch` 1304 checks / 0 failures (MinGW GCC 15.2, CMake/Ninja).
- No prior AARCH64-3 implementation or acceptance evidence existed
  (`git log --all --grep=AARCH64-3` empty, no `*pc_stack*` files).

## 2. Implemented instructions (production pipeline only)

All new forms decode in `Cpu::decodeWord` and execute in
`Cpu::executeDecoded(IMemory&)` — the same fetch/decode/execute path as
AARCH64-1/AARCH64-2. No separate interpreter, no mock execution.

### ADR — `op immlo 10000 immhi Rd` (`(word & 0x1F000000) == 0x10000000`)

- `imm21 = immhi:immlo` = `(immhi << 2) | immlo`, sign-extended from 21 bits
  with the existing `signExtend()` helper (range −1048576 … +1048575).
- Result: `Xd = PC + SEx(imm21)` — PC is the address of **this** instruction
  (`pc_` still points at it during execute), never PC+4. Unsigned addition
  wraps mod 2^64.
- `Rd == 31` is XZR (result discarded), never SP. NZCV preserved. PC += 4.

### ADRP — same layout with `op == 1` (`== 0x90000000`)

- Distinct formula by design (not shared with ADR): pre-scaled displacement
  `pcOffset = SEx(imm21) * 4096` (range −0x100000000 … +0xFFFFF000).
- Result: `Xd = (PC & ~0xFFF) + pcOffset` — 4 KiB page-relative.
- `Rd == 31` is XZR. NZCV preserved. PC += 4.
- Page-boundary property tested: PCs `0x1000…0x1FFC` all resolve page `0x1000`.

### LDR literal — `opc 011000 imm19 Rt`

- `0x18` family (`opc == 00`): `LDR Wt`; `0x58` family (`opc == 01`): `LDR Xt`.
- Address: `PC + SEx(imm19)*4` (pre-scaled `pcOffset`, range ±1 MiB).
- Real little-endian guest-memory read through `IMemory` (host-endian
  independent assembly, same as AARCH64-2). 32-bit loads zero-extend into X.
- `Rt == 31` (WZR/XZR) still performs the access — a faulting address still
  reports `Fault::MemoryFault`. Failed loads preserve destination and PC.
- NZCV preserved. PC += 4 after success.
- `LDRSW` (`0x98` family), `PRFM` (`0xD8`), and SIMD&FP literal families
  (`0x1C/0x5C/0x9C/0xDC`) do **not** match the decode masks and remain
  explicitly unsupported (verified invalid, never misdecoded as integer LDR).

### MOVZ / MOVK — unchanged (AARCH64-1 implementation kept)

Validated for full-width synthesis instead of rewritten:

```asm
movz x0, #0x1234
movk x0, #0x5678, lsl #16
movk x0, #0x9abc, lsl #32
movk x0, #0xdef0, lsl #48     ; X0 = 0xDEF09ABC56781234
```

All four halfword positions, unchanged-halfword preservation, 32-bit
`hw>=2` rejection, W-form zero-extension, NZCV preservation, plus a realistic
`ADRP + ADD #imm` page-offset construction feeding STR/LDR, and a
MOVZ-built address used with memory operations.

### Decoder placement

ADR/ADRP decoding was proven disjoint from B/BL (`bits[31:26]` never
`000101/100101`), move-wide (`bits[28:23]` are `10000x`), add/sub, CBZ/CBNZ
and LDR/STR-unsigned-offset patterns; LDR-literal masks were proven disjoint
from CBZ/CBNZ and unsigned-offset bases. Order relative to those checks is
therefore irrelevant to correctness (commented in code). New `Op::{Adr,
Adrp, LdrLit}` values, one new `Decoded::pcOffset` field (fully scaled
signed byte displacement; branches keep `branchOffset`, LDR/STR-imm keep
`memOffset`). Disassembly: `adr/adrp xN, #0xADDR`, `ldr wN/xN, #0xADDR`.

## 3. Address-calculation reference

| Form | Base | Displacement | Example (`PC=0x1234`) |
|------|------|--------------|------------------------|
| ADR  | `PC` | `SEx(imm21)` | `ADR X0,#0` → `0x1234` |
| ADRP | `PC & ~0xFFF` | `SEx(imm21)*4096` | `ADRP X0,#0` → `0x1000` |
| LDR lit | `PC` | `SEx(imm19)*4` | `LDR X0,[PC,#8]` at `0x1000` → `[0x1008]` |

Wrap-around (mod 2^64) verified through the genuine execute path:
`PC=0xFFFFFFFFFFFFFFFC + ADR #4 → 0x0`; `ADRP` page `0xFFFFFFFFFFFFF000
+ 0x1000 → 0x0`; `LDR X` at wrapped target `0x0` loads real memory.

## 4. Stack-frame design (nested BL/RET)

Guest convention only (no claim of full AAPCS64; no hardware SP-alignment
fault introduced — alignment control stays deferred as in AARCH64-1/2):

- Frames use `SUB SP, SP, #16` / `ADD SP, SP, #16` (16-byte aligned at every
  public call boundary) plus `STR X30, [SP, #8]` / `LDR X30, [SP, #8]`.
- No STP/LDP, no pre/post-index (out of scope, as specified).
- Each nested function that overwrites X30 preserves the incoming return
  address in its own non-overlapping frame slot in real guest memory.

## 5. Integrated program A — nested subroutines

Machine-code layout (all addresses exact):

```text
main @ 0x1000:
  0x1000  movz x0, #0x8000        ; stack top
  0x1004  add  sp, x0, #0         ; establish guest SP = 0x8000
  0x1008  movz x2, #0x2000        ; result pointer
  0x100C  movz x1, #0
  0x1010  bl   0x101C             ; X30 := 0x1014
  0x1014  str  x0, [x2]           ; final result -> [0x2000]
  0x1018  b    0x1018             ; completion boundary (self loop)
function_A @ 0x101C:
  0x101C  sub  sp, sp, #16        ; SP 0x8000 -> 0x7FF0
  0x1020  str  x30, [sp, #8]      ; save 0x1014 at [0x7FF8]
  0x1024  movz x0, #0x1111
  0x1028  bl   0x103C             ; X30 := 0x102C
  0x102C  add  x0, x0, #0x22      ; consume function_B result
  0x1030  ldr  x30, [sp, #8]      ; restore 0x1014
  0x1034  add  sp, sp, #16        ; SP -> 0x8000
  0x1038  ret                     ; -> 0x1014
function_B @ 0x103C:
  0x103C  sub  sp, sp, #16        ; SP 0x7FF0 -> 0x7FE0
  0x1040  str  x30, [sp, #8]      ; save 0x102C at [0x7FE8]
  0x1044  movz x3, #0x1234
  0x1048  movk x3, #0x5678, lsl #16 ; X3 = 0x56781234
  0x104C  str  x3, [sp]           ; spill to own frame [0x7FE0]
  0x1050  ldr  x4, [sp]           ; reload
  0x1054  add  x0, x4, #0x10      ; X0 = 0x56781244
  0x1058  ldr  x30, [sp, #8]      ; restore 0x102C
  0x105C  add  sp, sp, #16        ; SP -> 0x7FF0
  0x1060  ret                     ; -> 0x102C
```

Execution results (verified by single-step milestone trace of all 24 PCs,
plus a bounded `run(mem, 24)`):

- Steps to completion: **24** (`stepsExecuted == 24`, no fault, `PC == 0x1018`).
- Final: `PC=0x1018`, `SP=0x8000`, `X0=0x56781266`, `X30=0x1014`,
  `[0x2000]=0x56781266`, NZCV `= 0` throughout.
- Return path: `BL@0x1010 → 0x101C (X30=0x1014)` →
  `BL@0x1028 → 0x103C (X30=0x102C, SP=0x7FE0)` →
  `RET@0x1060 → 0x102C (SP=0x7FF0)` → `RET@0x1038 → 0x1014 (SP=0x8000)`.
- Stack slots after return: `[0x7FF8]=0x1014`, `[0x7FE8]=0x102C`,
  `[0x7FE0]=0x56781234` (restores do not erase; frames do not overlap).
- Canaries: `[0x7F00,0x7FE0) = 0xCC` and `[0x8000,0x8100) = 0xDD` intact.
- Determinism: two runs from identical initial state give identical
  registers/SP/PC/PSTATE and identical FNV-1a memory hash.
- `b .` at `0x1018` self-loops (one extra step keeps `PC == 0x1018`).
- No host-side X30/SP adjustment between instructions (step-only execution).

Negative coverage: invalid-SP store/load faults with PC+regs held;
misaligned `RET` target (`0x1001`) succeeds then faults `UnalignedPC` on next
fetch; corrupted saved X30 (`0xFFFF0000`) and missing X30 (zero) both fault
deterministically with no host crash; guard pair — exact-fit `STR W` at
`top-4` succeeds while 8-byte `STR X` crossing the end faults; `run(mem, 3)`
bounds a `b .` loop; XZR destinations never alias SP.

## 6. Integrated program B — PC-relative addressing and literals

Position-independent image (code + embedded literal pool + separate data
area at `+0x80`), executed at two bases with different page offsets:

- `C1 = 0x4000` (page offset `0x000`), `C2 = 0x6120` (page offset `0x120`).
- `ADR X0` → `C+0x80` (byte address); `ADRP X1` → `(C+0x80) & ~0xFFF`
  (page displacement computed from the real instruction/page addresses, so
  the builder stays correct for any base); backward `LDR W/X` literals from
  an embedded pool skipped via `B`; MOVZ/MOVK full-width constant;
  STR/LDR round-trip through the ADR-built address; `B .` terminator.
- 15 steps to `PC = C+0x4C`, no fault, NZCV `= 0`.
- Relocation verified: `X0` shifts by the full delta `0x2120`, `X1` (page
  base) by the page delta `0x2000`, literal/constructed values
  (`0xA5A55A5A`, `0x1122334455667788`, `0xDEF09ABC56781234`) invariant, and
  stored/loaded-back values match at both bases.
- Relocation property (documented, not an ELF loader): byte addresses from
  ADR and literal values relocate transparently when code+data move
  together; ADRP page displacement is invariant only while the code↔data
  relative page layout is preserved — changing it needs re-encoding, which
  this phase deliberately does not do.

## 7. Plugin and serialization compatibility

- `AArch64ISAPlugin` decode/execute/step/disassemble/state-inspection all
  cover the new forms through `IMemory` (no private CPU helpers).
- `hasFeature`: added `ADR`, `ADRP`, `LDR_LIT`; still rejects `STP`, `LDP`,
  `LDRSW`, `MOVN`, `GIC`. Version string advanced to
  `"AARCH64-3 (ARMv8-A A64 subset)"` (no test pins the old string).
- Serialization: **unchanged 280-byte v1 layout** (`8 + 31*8 + 8 + 8 + 4 +
  4`; `kSerializedStateSize == 280` asserted in tests). No new architectural
  state was needed.
- Hand-built v1 blob (magic `A641`, version 1, X5/SP/PC/NZCV) loads exactly;
  bad magic, version 2, short (100 B) and oversize (281 B) blobs are
  rejected with zero partial mutation (dump-compare before/after).
- Independent plugin instances step different programs without crosstalk.
- Registry: `AArch64` registered; `createISA("AArch64", nullptr)` valid
  (still no IA-64 firmware/decoder required); IA-64 gate unchanged.

## 8. Test commands and results (exact)

Toolchain: MinGW GCC 15.2, CMake 4.3.1, Ninja 1.13.2, Release.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target test_aarch64_foundation test_aarch64_memory_branch test_aarch64_pc_stack test_efi_memory_map test_ia64_sal test_riscv_decoder
ctest --test-dir build -R "AArch64|EfiMemoryMap|IA64Sal|RiscVDecoder" --output-on-failure
git diff --check
```

Results:

| Suite (CTest name) | Binary checks | Status |
|--------------------|---------------|--------|
| `AArch64FoundationTests` (AARCH64-1) | 221 checks, 0 failures | Passed |
| `AArch64MemoryBranchTests` (AARCH64-2) | 1304 checks, 0 failures | Passed |
| `AArch64PcStackTests` (new AARCH64-3) | 940 checks, 0 failures | Passed |
| `EfiMemoryMapTests` | — | Passed |
| `IA64SalTests` | — | Passed |
| `RiscVDecoderSmokeTest` | — | Passed |
| `git diff --check` | — | Clean (exit 0) |

`ctest`: **6/6 passed, 0 failed** (total 0.41 s).
New suite: `AARCH64-3 pc/stack: 940 checks, 0 failures` (exit 0).

Pre-existing, out-of-gate observation (unchanged by this phase):
`tests/test_isa_plugin.cpp:1551` (IA-64 NaT serialization assert) remains a
documented pre-existing failure; no IA-64 file was modified
(`git status` shows only `CMakeLists.txt`, `include/aarch64/*`,
`src/aarch64/*`, new `tests/test_aarch64_pc_stack.cpp`, new doc). The
`test_isa_plugin` binary is a very long-running IA-64 suite (still emitting
passing sections after 300 s); it is not one of the required gates and was
not altered to chase it.

## 9. Unsupported architecture features (still out of scope)

LDRSW/PRFM/SIMD&FP literals, MOVN, STP/LDP, pre/post-indexed and unscaled
forms, CCMP, shifted/extended-register arithmetic, MMU/translation,
EL transitions, GIC, UART/devices, SIMD/FP, SMP, JIT, full AAPCS64,
UEFI/Linux boot, GUI selection, IA-64 refactoring. Unsupported encodings
fault deterministically — never silent NOPs.

## 10. Recommended AARCH64-4 scope

1. `STP`/`LDP` (signed 7-bit offset, 32/64-bit) to express idiomatic
   frame save/restore, reusing the AARCH64-3 stack tests as callers.
2. `LDUR`/`STUR` (unscaled 9-bit offset) and pre/post-index addressing.
3. Logical immediates (`AND/ORR/EOR`, `MOV` bitmask alias) with NZCV rules.
4. `CSEL`/`CSINC` family + `TST` alias to widen conditional-code coverage.
5. Deeper call chains (3+ nesting) and a larger relocatable multi-section
   image still within the flat-memory profile (no MMU/ELF loader).

## 11. Files changed

- `include/aarch64/AArch64Cpu.h` — `Op::{Adr,Adrp,LdrLit}`, `Decoded::pcOffset`, scope/encoding docs.
- `src/aarch64/AArch64Cpu.cpp` — ADR/ADRP/LDR-literal decode + execute + disassembly.
- `include/aarch64/AArch64ISAPlugin.h` — version `AARCH64-3`.
- `src/aarch64/AArch64ISAPlugin.cpp` — `ADR/ADRP/LDR_LIT` features.
- `tests/test_aarch64_pc_stack.cpp` — new suite (940 checks).
- `CMakeLists.txt` — additive `test_aarch64_pc_stack` target + `AArch64PcStackTests`.
- `docs/aarch64/AARCH64_3_PC_STACK.md` — this document.
- `docs/aarch64/.phase` — advanced to `last_completed=AARCH64-3`.

Architectural references: ARM DDI 0487 (ARMv8-A ARM); mirrors at
https://arm.jonpalmisc.com/latest_aarch64/adr,
…/adrp, …/ldr_lit_gen, …/bl, …/ret; encodings independently anchored
(§2) rather than copied.
