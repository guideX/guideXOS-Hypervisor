# PPC64 Phase 2 — Fixed-Point Arithmetic, Condition Codes, and Branches

Branch: `PPC64`. Phase marker: `.phase-ppc64` (`last_completed=PPC64-2`,
`next_expected=PPC64-3`; the shared IA-64 `.phase` file is untouched).

Primary semantic reference: OpenPOWER Power ISA v3.1C, Book I (fixed-point
facility and branch facility), plus IBM AIX assembler-language references
for the BO-field tables and NXP MPCxxx manuals for D-form pseudocode.
This document claims only the subset below — **not** complete Power ISA
compatibility.

## 1. Exact new instruction coverage

All encodings are big-endian 32-bit, executed through the production
`PPC64ISAPlugin` (decode + execute + `step`) against real `Memory`.

| Mnemonic | Format | Encoding | Semantics (64-bit) |
|---|---|---|---|
| `add[o][.] RT,RA,RB` | XO | 31, XO 266 | `RT := RA + RB` (wrap); OE→OV/SO; Rc→CR0 |
| `subf[o][.] RT,RA,RB` | XO | 31, XO 40 | `RT := RB - RA` (wrap); OE→OV/SO; Rc→CR0 |
| `addic RT,RA,SI` | D | 12 | `RT := (RA) + EXTS(SI)`; CA set; no CR0 |
| `addic. RT,RA,SI` | D | 13 | as `addic`, plus CR0 |
| `addc[.] RT,RA,RB` | XO | 31, XO 10 | `RT := RA + RB`; CA set; Rc→CR0 |
| `adde[.] RT,RA,RB` | XO | 31, XO 138 | `RT := RA + RB + CA`; CA in/out; Rc→CR0 |
| `subfc[.] RT,RA,RB` | XO | 31, XO 8 | `RT := RB - RA`; CA:=¬borrow; Rc→CR0 |
| `and[.] RA,RS,RB` | X | 31, XO 28 | `RA := RS & RB`; Rc→CR0 |
| `or[.] RA,RS,RB` | X | 31, XO 444 | `RA := RS \| RB`; Rc→CR0 |
| `xor[.] RA,RS,RB` | X | 31, XO 316 | `RA := RS ^ RB`; Rc→CR0 |
| `andi. RA,RS,UI` | D | 28 | `RA := RS & zeroext(UI)`; always CR0 |
| `andis. RA,RS,UI` | D | 29 | `RA := RS & (UI<<16)`; always CR0 |
| `xori RA,RS,UI` | D | 26 | `RA := RS ^ zeroext(UI)`; never CR0 |
| `xoris RA,RS,UI` | D | 27 | `RA := RS ^ (UI<<16)`; never CR0 |
| `cmp BF,L,RA,RB` | X | 31, XO 0 | signed compare → CR[BF] |
| `cmpl BF,L,RA,RB` | X | 31, XO 32 | unsigned compare → CR[BF] |
| `cmpi BF,L,RA,SI` | D | 11 | signed compare vs EXTS(SI) → CR[BF] |
| `cmpli BF,L,RA,UI` | D | 10 | unsigned compare vs UI → CR[BF] |
| `bc/bca/bcl/bcla BO,BI,disp` | B | 16 | conditional branch, BD-based target |
| `bclr/bclrl BO,BI` | XL | 19, XO 16 | branch to `LR & ~3` |
| `bcctr/bcctrl BO,BI` | XL | 19, XO 528 | branch to `CTR & ~3` |
| `mfspr RT,SPRN` | XFX | 31, XO 339 | SPRN ∈ {1 XER, 8 LR, 9 CTR} |
| `mtspr SPRN,RS` | XFX | 31, XO 467 | SPRN ∈ {1 XER, 8 LR, 9 CTR} |

Anchor cross-checks asserted in tests: `blr=0x4E800020`,
`bctr=0x4E800420`, `mflr r0=0x7C0802A6`, `mtlr r0=0x7C0803A6`,
`add r3,r4,r5=0x7C642A14`, `beq .+8=0x41820008`, `cmpwi r3,0=0x2C030000`.

### Supported and unsupported forms

- Supported record variants: `add[.]`, `addo[.]`, `subf[.]`, `subfo[.]`,
  `addc[.]`, `adde[.]`, `subfc[.]`, `addic.`, `and[.]`, `or[.]`, `xor[.]`,
  `andi.`, `andis.`. Non-record `xori`/`xoris`/`ori`/`oris` never touch CR0.
- Out of scope → deterministic `EXCEPTION` (never approximate results):
  OE=1 on `addc`/`adde`/`subfc` (`addco`, `addeo`, `subfco` — valid per ISA
  but not implemented); Rc=1 on `cmp`/`cmpl`/`mfspr`/`mtspr`; reserved ISA
  bit 9 set on `cmp`/`cmpl`/`cmpi`/`cmpli`; `bcctr` with the decrement-CTR
  BO form (`BO[2]==0` — architecturally invalid); any SPR outside {1,8,9}
  (including privileged SPRs such as HID0=1008); unknown XO values;
  unknown primary opcodes.
- BH hint bits on `bclr`/`bcctr` and `y`/`a`/`t` hint bits in BO are
  accepted and ignored functionally (documented; matches hardware
  ignoring hints).
- Phase 1 behavior preserved: `addi`/`addis` (RA=0 → zero),
  `ori`/`oris`/`nop`, `b`/`bl`/`ba`/`bla`, 304-byte serialization layout,
  `getWordSize()=64`, big-endian fetch, `PPC64` registry factory.

## 2. CR0 and XER rules implemented

- CR holds 8 independent 4-bit fields `{LT,GT,EQ,SO}`, MSB-first. CR bit
  `BI` is u32 bit `(31-BI)`; field `BF` is u32 bits `(28-4*BF)+[0..3]`.
  All access goes through `crGetBit`/`crGetField`/`crSetField` — the only
  place CR bit numbering exists.
- XER stores `SO=0x80000000`, `OV=0x40000000`, `CA=0x20000000` (the low
  32 bits of the architectural XER, i.e. XER[32:63]).
- Record form (`Rc=1`, `addic.`, `andi.`/`andis.`): CR0 from the **wrapped**
  result — `LT=result<0`, `GT=result>0`, `EQ=result==0` (signed 64-bit),
  `SO=XER.SO` sampled **after** any OE update. Non-record forms never
  modify CR.
- `OE=1` (`addo`, `subfo`): `OV` from unsigned-pattern signed-overflow
  formulas (`add: ((a^r)&(b^r))>>63`; `sub: ((b^a)&(b^r))>>63` — no host
  signed-overflow UB); `SO := SO | OV` (sticky; `OV` clears on clean
  operations while `SO` persists).
- Carry: `addc`/`adde`/`addic`/`addic.` set `CA` from the unsigned carry
  out (`result < a` Limb comparison; `adde` ORs both half-addition
  carries with the incoming `CA` consumed as operand); `subfc` sets
  `CA := (RB >= RA)` (NOT borrow). Plain `add`/`subf` never touch `CA`.
- `addic` uses plain `(RA)`: RA=0 reads **GPR0** (unlike `addi`/`addis`
  where RA=0 selects constant zero) — verified against ISA pseudocode
  (`rD ← (rA) + EXTS(SIMM)`).
- Comparisons: `L=1` full 64-bit; `L=0` low-32-bit with signed (`cmp`,
  sign-extended `cmpi` operand) vs unsigned (`cmpl`, zero-extended
  `cmpli` operand) interpretation; target field `BF` set to
  `{LT,GT,EQ}` plus `SO=XER.SO`; all other CR fields preserved.

## 3. Branch BO/BI semantics

MSB-first BO numbering (`BO[0]`=0x10 … `BO[4]`=0x01), per Book I:

- Decrement CTR iff `BO[2]==0` (0x04 clear). Decrement happens **first**
  (wrapping mod 2⁶⁴) and is **not** conditional on the branch being taken.
- `ctr_ok := BO[2] | (((CTR==0) == BO[3]))` on the decremented value.
- `cond_ok := BO[0] | ((CR[BI] == BO[1]))`.
- Taken iff `ctr_ok && cond_ok`.
- `bc`: target `CIA + sext(BD||00)` (14-bit BD), or absolute `sext(BD||00)`
  for `bca`; `LK=1` writes `LR := CIA+4` even when not taken; fall-through
  is `CIA+4`.
- `bclr`: target captured as `LR & ~3` **before** any `LK` update; CTR
  decrement/test per BO (e.g. `bdnzlr`); `bclrl` overwrites LR after
  capturing. `bcctr`: target `CTR & ~3`; `BO[2]==0` rejected at decode
  as invalid; `bcctrl` writes LR. Tested: taken/not-taken × absolute/
  relative × LK × CTR∈{0,1,>1,wrap} × CR-only/CTR-only/combined/unconditional.

## 4. Serialization compatibility results

- Format **unchanged**: 304-byte v1 payload (`PPC1`/1/GPRs/NIP/LR/CTR/CR/
  XER/MSR). No version bump needed — all Phase 2 state already had homes.
- A hand-built Phase 1 blob (GPR3=42, NIP=4, LR=0x24, CTR=7, CR=GT,
  XER=CA) restores exactly under Phase 2; truncated/empty/bad-magic/
  bad-version blobs are rejected with state intact.
- Round trip with nonzero CR/XER/LR/CTR verified.

## 5. Three complete execution-program results

All programs execute real encoded instructions via `step()` on the
production plugin; all checks passed.

**Program A — arithmetic and flags** (base 0x100):
`addi r3,r0,100` → r3=100; `addi r4,r0,-1` → r4=0xFFFF…FF;
`addc r5,r3,r4` → r5=99, CA=1, CR=0 (Rc=0);
`adde r6,r3,r3` → r6=201 (100+100+CA), CA=0;
`subfc. r7,r3,r6` → r7=101, CA=1, CR0=GT;
`andi. r8,r7,0xFF` → r8=101, CR0=GT; `nop` → final NIP=0x11C.

**Program B — conditional loop** (base 0x200): sum 1..5 with
`add`/`addi`/`cmpw cr0,r4,r5`/`bne(-12)`. 5 iterations, 24 steps total
(3 setup + 5×4 body + trailing nop); final r3=15, r4=6, CR0=EQ,
NIP=0x220.

**Program C — call and return** (base 0x300): `addi r3,r0,10`;
`bl 0x310` (LR=0x308); subroutine `addi r3,r3,5` → r3=15;
`blr` → NIP=0x308; `b 0x31C` skips over; CTR-indirect leg
(`addi r6,r0,0x340` + `mtctr r6` + `bctr` → NIP=0x340, LR preserved
at 0x308) then `addi r7,r0,77` → r7=77, final NIP=0x348.

## 6. Test counts and regressions

- New `tests/test_ppc64_phase2.cpp`: 14 groups, **399 checks, 0 failures**.
- Phase 1 suite still **148 checks, 0 failures** (one probe encoding
  updated, see §7; check count unchanged).
- CTest: 8/14 pass — `BasicTests`, `NewInstructionTests`,
  `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`,
  `RiscVDecoderSmokeTest`, `PPC64Phase1Tests`, **`PPC64Phase2Tests`** (new).
  The 6 `Not Run` entries (`SyscallTests`, `DebuggerTests`,
  `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`,
  `ELFValidationTests`) are the **pre-existing** MinGW `ws2_32` link
  failures documented in Phase 1 (re-verified: `test_debugger` still
  fails on `__imp_send` in untouched `WebUIServer.cpp`); intentionally
  not repaired in this phase.
- Standalone IA-64 binaries still pass: `test_ia64_interrupt_controller`,
  `test_ia64_processor_interrupt_block`.
- Known unrelated legacy failure (not repaired, not caused here):
  `test_isa_plugin` NaT serialization abort.
- No PPC toolchain or QEMU available in this environment, so encodings
  are hand-built and cross-checked against published anchors (§1);
  differential QEMU verification remains future work.

## 7. Discovered defects and repairs

1. **XO numbering (implementation bug, caught by tests):** the first
   cut decoded the arithmetic family with halved XO values (e.g. 133
   instead of the table value 266) because of a 9-bit vs 10-bit field
   confusion; the X-form logical tests (which used true table values
   28/444/316) failed. Fixed by adopting the uniform convention
   `combined = (word>>1) & 0x3FF` compared directly against IBM table
   values (266/40/10/138/8), with OE as bit 9. Test anchors updated to
   true values (`add r3,r4,r5=0x7C642A14`).
2. **Phase 1 test maintenance:** `tests/test_ppc64_phase1.cpp` probed
   `0x7C000000` as "unsupported X-form", but that word is the valid
   `cmpw cr0,r0,r0` once Phase 2 implements compares. The probe now uses
   `0x7C000002` (XO=1, still unimplemented). No Phase 1 behavior changed;
   148/148 preserved.
3. **MSVC portability:** carry computation switched from
   `unsigned __int128` to portable `result < a` limb comparisons (also
   silences `-Wpedantic`).
4. **Test-expectation slips (tests only):** `andis.` expectation
   corrected to `0x340000`; `addic.` −5 expectation corrected to
   `0xFFFFFFFFFFFFFFFB`; sticky-SO subf sequence reordered so the
   reset does not wipe the SO under test.

## 8. Remaining gaps toward a useful PPC64 guest environment

- No loads/stores, no MMU/address translation, no exception model beyond
  decode/fetch `EXCEPTION` (PPC64-3 territory).
- No floating-point, vector, or string/multiple fixed-point ops; no
  `mcrf`/CR-logical instructions; no `sc`/syscall surface.
- Privileged SPRs/MSR semantics, SMP, firmware (sPAPR/OPAL), hypercalls,
  devices, and full-`VirtualMachine` hosting all still deferred; the
  IA-64-centric VM decoder/scheduler/checkpoint assumptions are
  untouched, as required.
- `addco`/`addeo`/`subfco` (valid ISA, OE=1 on carry/extended forms)
  explicitly rejected — implement if a guest needs them.
- Differential verification against QEMU PPC64 pending toolchain
  availability.
