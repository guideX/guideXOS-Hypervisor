# AARCH64-2 — Memory Access, Conditional Branching & NZCV Arithmetic

Status: implemented and validated (pending acceptance). Branch: `AARCH64`.
Root IA-64 `.phase` untouched (`last_completed=IA64-POST-MADT-6`);
AArch64 tracking lives in `docs/aarch64/.phase` only.

## 1. Original baseline

- Starting commit `cb6da08` (AARCH64-1, accepted, Outcome A, 219/219 checks).
- Preflight (2026-10-14): branch `AARCH64`, HEAD `cb6da08`, clean tree,
  upstream `origin/AARCH64` in sync. Baseline rebuilt and re-run before any
  edit: `AARCH64-1 foundation: 219 checks, 0 failures`.
- No prior AARCH64-2 work present (clean tree; only AARCH64-1 files).

## 2. Architectural references

- ARM Architecture Reference Manual ARMv8-A (ARM DDI 0487): C6.2 (add/sub
  immediate, load/store immediate, move wide), C6.6 (branches), B2 (W
  zero-extension), C1.2 (ConditionHolds semantics).
- Machine-readable cross-checks (mirrors of the authoritative ISA XML):
  - `https://arm.jonpalmisc.com/latest_aarch64/ret` — operation is
    `target = X(n)` (general-purpose view; X(31) is zero, not SP).
  - `.../ldr_imm_gen`, `.../str_imm_gen` — unsigned-offset decode
    `offset = LSL(ZeroExtend(imm12), scale)`; `Rn == 31` is SP with
    `CheckSPAlignment`; `X[t]` destination (X(31) discarded); no writeback.
  - `.../b_cond` (+ Stanford mirror) — decode
    `offset = SignExtend(imm19:'00')`, `condition = cond`; operation calls
    `ConditionHolds(condition)`.
  - Shared pseudocode `ConditionHolds(cond)` quoted verbatim in
    `src/aarch64/AArch64Cpu.cpp` (`conditionHolds()`): `cond[3:1]` selects
    the base condition (`111 → TRUE`), and bit 0 inverts **except** for
    `cond == 0b1111`. Hence `AL (1110)` and `NV (1111)` **both always hold**
    — they are not inverses.
- `https://support.arm.com/documentation/102376/latest/` (alignment and
  endianness): unaligned ordinary accesses to Normal memory are permitted
  when alignment checking is disabled.

## 3. New instruction families (all really executed)

| Family | Forms | Semantics |
|---|---|---|
| LDR (unsigned off.) | Wt/Xt, imm12 any | `Xt = ZeroExtend(Mem[base + (imm12<<2/3)])`, LE; Rn31=SP, Rt31=XZR/WZR; no writeback; NZCV untouched |
| STR (unsigned off.) | Wt/Xt, imm12 any | `Mem[base + (imm12<<2/3)] = Xt`, LE, width-exact; Rn31=SP, Rt31=zero source (still accesses); NZCV untouched |
| CBZ/CBNZ | Wt/Xt | `offset = SEx(imm19)<<2` from current PC; Rt31=zero (CBZ always / CBNZ never); NZCV untouched |
| ADDS/SUBS imm | W/X, imm12 any, LSL 0/12 | result + full NZCV (unsigned carry/borrow, signed overflow, unsigned-only host arithmetic); Rn31=SP, Rd31=XZR/WZR |
| CMP imm | W/X (alias) | SUBS with Rd31: NZCV updated, nothing written |
| B.cond | cond 0–15 | `ConditionHolds()` verbatim; AL and NV both always branch; bit-4-set and other mismatches are `UnallocatedEncoding`; NZCV untouched |

New `Fault::MemoryFault` covers backing-store data-access failures
(out-of-range `Read`/`Write`). Fetch failures stay `InvalidFetch`.

## 4. Encoding and decoding decisions

- LDR/STR unsigned offset: `bits[31:22]` matchers `0xB9400000` (Wt),
  `0xF9400000` (Xt), `0xB9000000` (Wt), `0xF9000000` (Xt); fields
  `imm12[21:10] Rn[9:5] Rt[4:0]`; `memOffset = imm12 << (sf ? 3 : 2)`.
  Anchored: `ldr x0,[x1] = 0xF9400020`, `str x0,[x1] = 0xF9000020`,
  `ldr w3,[x2] = 0xB9400043`, `str w2,[x3,#8] = 0xB9000862`.
- CBZ/CBNZ: `bits[30:24]` `011010` + op bit 24 (`0x34/0x35`, sf in bit 31);
  `imm19[23:5] Rt[4:0]`. Anchored: `cbz x0,. = 0xB4000000`,
  `cbnz w1,.+8 = 0x35000041`.
- ADDS/SUBS/CMP: existing add/sub matcher plus `S` bit; `SUBS`+`Rd31` decodes
  as distinct `Op::CmpImm` (disassembles `cmp`, never `subs`). Anchored:
  `adds x0,x1,#1 = 0xB1000420`, `cmp x1,#1 = 0xF100043F`.
- B.cond: `(word & 0xFF000010) == 0x54000000` (top byte `0x54`, bit 4 must be
  0); `cond = bits[3:0]`, `imm19[23:5]`. Bit 4 set (e.g. `0x54000010`) is
  `UnallocatedEncoding`, never a NOP. Anchored: `b.eq . = 0x54000000`.
- All new matchers verified pairwise-disjoint from B/BL, RET, MOVZ/MOVK,
  ADD/SUB and NOP, and from each other.
- `Decoded` gains `memOffset` (scaled LDR/STR offset) and `cond` (B.cond);
  `branchOffset` is reused for CBZ/CBNZ/B.cond (all are PC-relative ±4-scaled).
- `Cpu::step()` now forwards guest memory into
  `executeDecoded(decoded, memory)` (previously the memory-less overload was
  used, which would fault every data access). The single-argument overload is
  retained for AARCH64-1 call-site compatibility and deterministically reports
  `MemoryFault` for data accesses via a storage-less `IMemory`.
- Plugin `execute()` forwards `IMemory` (previously ignored); `hasFeature()`
  gains `LDR_IMM/STR_IMM/CBZ/CBNZ/ADDS_IMM/SUBS_IMM/CMP_IMM/B_COND`;
  version string is now `AARCH64-2 (ARMv8-A A64 subset)`.

## 5. Memory access and alignment policy (AARCH64-2 profile)

Explicit and bounded: little-endian guest data access through the generic
`IMemory` contract; Normal memory; no MMU; no Device mapping; no EL-specific
system registers. Consequences, all tested:

- Architecturally permitted **unaligned ordinary LDR/STR accesses succeed**
  (W at odd bases, X crossing 8-byte lines, nonzero scaled offsets).
  Rationale: ARM permits them for Normal memory when alignment checking is
  disabled, and there is no SCTLR_ELx.A control in this profile — so
  EL-dependent alignment control is **deferred and documented**, not silently
  enforced. `SCTLR_ELx.A/SA` and `CheckSPAlignment` remain unimplemented.
- Instruction fetch still requires 4-byte alignment (`UnalignedPC`).
- Address arithmetic wraps mod 2^64 (`base + memOffset`); a wrapped address
  that lands in range is a valid access (tested: `0xFFFF..FF + 8 → [7]`);
  a wrapped address still out of range is `MemoryFault` (no host crash).
- Failed loads change **neither** the destination register **nor** PC;
  rejected stores report `MemoryFault` without advancing PC. The guarantee is
  scoped to the single bounded `Read`/`Write` call — no global
  transactionality beyond the `IMemory` contract is claimed (bounds-checked
  implementations reject before writing; verified with canary-filled
  `FlatMemory`: neighbors of faulting stores stay `0xA5`).
- 32-bit loads zero-extend (never sign-extend); W stores write exactly the
  low 4 bytes (neighbors preserved); XZR/WZR loads still perform the access
  (bad address faults) while discarding the value; XZR/WZR stores write
  architectural zeros.

## 6. Flag-setting arithmetic behavior

`ADDS/SUBS/CMP` immediates compute NZCV with unsigned host arithmetic only
(no signed-overflow UB): 64-bit carry = `r < a`, borrow-not = `a >= b`;
32-bit carry from the 33rd bit of a `uint64_t` sum; overflow from the sign
bit of `(~(a^b))&(a^r)` (add) / `((a^b)&(a^r))` (sub), at bit 63/31.
Covered: zero, wrap carry (`0xFF..FF+1 → Z,C`), borrow (`0-1 → N,~C`),
maximum unsigned operands, signed-overflow boundaries in both widths
(`INT64_MAX+1 → N,V`; `INT64_MIN-1 → V`; `INT32_MAX+1 → N,V`;
`INT32_MIN-1 → V`), `LSL #12` addends including `0xFFF000`, SP-as-Rn in both
widths (32-bit uses low 32 of SP), Rd31 discard-with-flags, CMP writing
nothing, and preservation of non-flag-setting ADD/SUB (NZCV untouched, SP
destination and wrap behavior intact).

## 7. Conditional branch semantics

`conditionHolds()` implements the ARM pseudocode verbatim, tested taken and
not-taken for all of EQ/NE/CS/CC/MI/PL/VS/VC/HI/LS/GE/LT/GT/LE, plus:

- `AL` taken under NZCV=0000 and NZCV=1111; `NV` taken under both too —
  the naive "odd cond inverts" reading would make NV never-branch; the
  architecture explicitly excludes `0b1111` from inversion.
- Signed-vs-unsigned discrimination at an overflow boundary: after
  `SUBS x2, x1, #1` with `x1 = 0x8000000000000000` (N=0,Z=0,C=1,V=1),
  `B.HI` takes (unsigned above) while `B.GE` does not (signed less-than).
- Not-taken advances PC by exactly 4; NZCV never modified; targets are
  relative to the current instruction address (`±1MB`, boundary-tested);
  taken-to-unmapped targets surface on the next fetch through the existing
  B/BL/RET model (`InvalidFetch`); CBZ-from-aligned-PC targets are always
  4-aligned by construction (offsets scale by 4).

## 8. RET register-31 audit result (Audit A)

**The AARCH64-1 implementation was incorrect and has been repaired.**
It read SP for `RET` with encoded register 31 (and the foundation test
asserted that behavior). The authoritative definition
(`RET` operation: `target = X(n)`, "the 64-bit name of the general-purpose
register") uses the general-purpose view, where register 31 is XZR (zero),
not SP. Changes:

- `executeDecoded` RET case: `rn == 31 → target 0` (was: `sp_`).
- Disassembly `ret xzr` (was `ret sp`); header/implementation comments
  corrected (`X(n)` operation cited).
- Foundation test updated: `RET 31` with SP=`0x5000` now expects PC=0 and
  SP undisturbed; follow-up fetch faults `UnallocatedEncoding` at address 0.
- Regression coverage added in both the foundation suite and the new
  memory/branch suite (link-register, arbitrary-register, XZR-target,
  disassembly, misalignment paths).

No correct behavior was changed for ambiguous documentation: `ADD/SUB`
(`Rn/Rd31 = SP`), `ADDS/SUBS` (`Rn31 = SP`, `Rd31 = XZR/WZR`), LDR/STR
(`Rn31 = SP`, `Rt31 = XZR/WZR`), and CBZ/CBNZ (`Rt31 = zero`) each follow
their own instruction-specific rule, all covered by tests. (Pre-existing
scope note: pointer-authentication RET variants decode as plain RET under
the AARCH64-1 mask; unchanged and out of scope.)

## 9. Integrated program traces (production pipeline, bounded, no poking)

Program 1 — SUBS/B.cond memory loop (`movz x0,#5; movz x1,#0x2000;
str w0,[x1]; ldr w2,[x1]; subs w2,w2,#1; str w2,[x1]; b.ne; ldr w3,[x1];
nop`, code `0x1000–0x1020`, data `0x2000`, canary-filled memory):

- 25 instructions executed, 4 branches taken, 0 faults; final PC `0x1024`.
- `x0=5`, `x1=0x2000`, `x2=0`, `w3=0`, SP=0, NZCV=`Z,C` (`pstate=0x60000000`
  from the final `1-1`).
- Memory `[0x2000,0x2004) = 0`; canaries `[0x1F00,0x2000)` and
  `[0x2004,0x2100)` intact; code region intact.
- Re-run from an identical initial state: identical registers, PC, PSTATE
  and FNV-1a memory hash.

Program 2 — CBZ/CBNZ program (32-bit `subs/cbnz` countdown from 4, 64-bit
`subs/cbnz` countdown from 3, two forward `cbz` skips over `nop`s,
code `0x3000–0x3028`):

- 19 instructions executed, 7 taken (3× CBNZ-W + 2× CBNZ-X + 2× CBZ),
  0 faults; final PC `0x302C`; `x0=x1=0`; NZCV=`Z,C`; deterministic re-run.

Both programs use explicit step budgets (100) and terminate by condition,
not by budget.

## 10. Test counts and results

Build: out-of-tree `D:/dev/build-aarch64-1`, MinGW GCC 15.2 + Ninja.

- `cmake -S <repo> -B <build> -G Ninja` — configure clean.
- `cmake --build <build> --target test_aarch64_foundation
  test_aarch64_memory_branch test_efi_memory_map test_ia64_sal
  test_riscv_decoder test_isa_plugin` — builds (only pre-existing
  third-party warnings, e.g. `-Wreorder`/`-Wunused-parameter` in
  `IA64ISAPlugin.*`; no new warnings from `src/aarch64/*`).
- `ctest -R "AArch64FoundationTests|AArch64MemoryBranchTests|
  EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest"` — **5/5 pass**.
- Direct runs: `AARCH64-1 foundation: 221 checks, 0 failures`
  (219 baseline + 2 net-new audit checks); `AARCH64-2 memory/branch:
  1304 checks, 0 failures` (encoders, RET audit, LDR/STR, widths, reg-31,
  unaligned, faults, CBZ/CBNZ, NZCV tables, B.cond incl. NV trap and
  signed/unsigned split, both integrated programs ×2 runs, isolation,
  plugin/serialization, IA-64 gate).
- `test_isa_plugin` rechecked: still aborts at the documented pre-existing
  assert `tests/test_isa_plugin.cpp:1551` (IA-64 state serialization,
  `GetGRNaTPhysical`); that binary links no AArch64 code and none of its
  sources were touched — left alone per scope.
- `git diff --check` — clean.

## 11. Known limitations

No Linux/UEFI boot, guest ISO loading, EL1/EL2 exceptions, MMU/page tables,
GIC, PL011, timers, SMP, PCIe/virtio, GUI selector, OS ABI, JIT, SIMD/FP,
atomics, or register-offset / pre/post-index / pair / unscaled / exclusive
memory forms (all still fault deterministically). No SP-alignment or
SCTLR-based alignment checking. Serialization stays at 280-byte v1
(verified compatible: hand-built v1 blobs load; note the pre-existing header
comment `magic={'A','6','4','1'}` describes intent while the code stores LE
bytes `31 34 36 41` — round-trips correctly, comment left untouched).
`test_isa_plugin:1551` remains a pre-existing IA-64 failure.

## 12. Next-phase recommendation (bounded AARCH64-3)

ADR/ADRP + LDR-literal for position-independent data access, `MOVK`-chained
address synthesis coverage, and one stacked-subroutine program (BL/RET
nesting with an SP-carried frame in guest memory); optionally SUBS-shifted-
register + CCMP for richer flag/condition coverage. Still no MMU, exceptions
or devices.
