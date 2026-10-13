# AARCH64-1 Foundation — Real A64 Execution Backend

Status: accepted. Architecture-local phase `AARCH64-1` complete.
Date: 2026-10-12. Branch: `AARCH64`. Root IA-64 `.phase` untouched
(still `last_completed=IA64-POST-MADT-6`); AArch64 tracking lives in
`docs/aarch64/.phase` only.

## 1. Mission outcome

A real, testable AArch64 (ARMv8-A, A64) execution backend now exists in
isolation alongside the working IA-64 implementation. It fetches, decodes
and actually executes a bounded A64 subset — not a stub, not a NOP-slide.

## 2. Architecture boundaries

New, isolated files (nothing else modified except a 30-line additive CMake
block):

- `include/aarch64/AArch64Cpu.h`, `src/aarch64/AArch64Cpu.cpp` — CPU state,
  fetch/decode/execute, bounded stepping.
- `include/aarch64/AArch64ISAPlugin.h`, `src/aarch64/AArch64ISAPlugin.cpp` —
  IISA plugin + `AArch64` registry auto-registration.
- `tests/test_aarch64_foundation.cpp` — 219-check independent suite.
- `docs/aarch64/` — this file + `.phase`.

Genuinely reusable as-is (verified by reading, not assumed):

- `include/IISA.h` — decode/execute/step over `IMemory`, PC access,
  serialize/restore, word size, endianness, feature queries. Fully
  architecture-neutral; implemented without modification.
- `include/ISAPluginRegistry.h` — name→factory map. The AArch64 factory
  adapter accepts and ignores the `void*` construction data, so
  `createISA("AArch64", nullptr)` is valid and never needs IA-64 objects.
  (Contrast: the IA-64 factory throws without `IA64FactoryData`+decoder —
  that behavior is preserved and gated by test.)
- `include/IMemory.h` — flat byte-addressable guest memory used for
  instruction fetch. No IA-64 assumptions exercised on this path.

Needed an adapter (used, not modified):

- Concrete `Memory` carries an IA-64-flavored MMU; tests use a small local
  `FlatMemory` implementing `IMemory` instead, keeping fetch semantics
  explicit (bounds → `InvalidFetch`).

Remains isolated (never constructed, included only as needed, unmodified):

- `IA64ISAPlugin` / `cpu_state.h` / IA-64 decoder / `VirtualMachine` /
  IA-64 firmware / GUI. No whole-VM boot is claimed: the plugin constructs
  and executes, but there is no BootStateMachine/GIC/MMU/UEFI path for
  AArch64 in this phase.

## 3. CPU state

- `X0–X30` stored as 31 distinct `uint64_t`; there is deliberately no
  32nd slot, so register 31 cannot be mistaken for an ordinary register.
- `SP` is a separate `uint64_t`; `PC` a separate `uint64_t` (byte address,
  must be 4-aligned at fetch or `UnalignedPC` faults).
- `PSTATE` is a `uint32_t` with NZCV in bits [31:28]; remaining bits are
  reserved-zero in this phase. Non-flag-setting ADD/SUB never touch NZCV
  (tested: flags survive arithmetic).
- `readX/writeX` accept only 0–30 and throw `std::out_of_range` for 31;
  `readW/writeW` expose the low half with zero-extension on W writes.
  Register-31 access goes through explicit `readReg31/writeReg31(is64,
  withZero)` helpers: `withZero=true` = XZR/WZR (reads 0, writes
  discarded), `false` = real SP.

## 4. Implemented instructions (all really executed)

| Mnemonic | Forms | Semantics |
|---|---|---|
| NOP | `0xD503201F` exactly | PC += 4. Other HINT encodings fault |
| MOVZ | 32/64-bit, hw 0–3 (64) / 0–1 (32) | Zero + place imm16; Rd=31 discards |
| MOVK | 32/64-bit, same hw rules | Keep all other bits; Rd=31 discards |
| ADD imm | 32/64-bit, imm12 any, shift 0/12, Rd/Rn=31 → SP | Wrap arithmetic; NZCV untouched |
| SUB imm | same | same |
| B | ±128 MB range | PC += SEx(imm26)<<2 |
| BL | same | X30 = PC+4, then branch |
| RET | any Rn incl. 31 → SP | PC = Xn (misaligned target faults on next fetch) |

Fetch: fixed 4-byte little-endian assembly independent of host endianness
(tested with raw byte patterns, including a transposed negative).

Faults (`Fault::None/UnalignedPC/InvalidFetch/UnallocatedEncoding/
UnsupportedFeature`): ADDS/SUBS, MOVN, non-NOP HINTs (e.g. SVC `#0`),
malformed 32-bit move-wide (`hw>=2`), and all other unallocated patterns
fault deterministically, preserve PC/state, and surface through IISA as
`EXCEPTION`. Unsupported is never a silent NOP.

Reset/entry: `reset(entry)` zeroes X/SP/PSTATE and sets PC=entry
(default 0) — documented as the initial emulator profile, not a claim
about physical ARM reset configurations.

Bounded stepping: `run(memory, maxSteps)` stops at the first fault or
after `maxSteps` (an infinite `b .` loop provably terminates by budget).

Serialization: fixed 280-byte LE layout (`A641` magic, version 1, X0–X30,
SP, PC, PSTATE, reserved). Bad magic/version/size → clean rejection with
prior state intact; foreign `ISAState` types rejected by `setState`.

## 5. Validation

Build (out-of-tree `D:/dev/build-aarch64-1`, MinGW GCC 15.2/MinGW Ninja):

- `cmake -S <repo> -B <build> -G Ninja` — configure clean.
- `cmake --build <build> --target test_aarch64_foundation` — builds
  (only pre-existing third-party warnings, e.g. unused params in
  `IStorageDevice.h`, `RestAPIServer.cpp`).
- `ctest -R AArch64FoundationTests` — **passed**; direct run:
  `AARCH64-1 foundation: 219 checks, 0 failures`.
- Test encoders anchored to reference encodings: NOP `0xD503201F`, MOVZ
  X0,#0 `0xD2800000`, B . `0x14000000`, BL . `0x94000000`, RET X30
  `0xD65F03C0` (ARM DDI 0487 field tables).

Regression gates (existing, no ISO/firmware needed):

- In-suite gate: `IA-64` still registered; `createISA("IA-64", nullptr)`
  still fails exactly as before (decoder requirement preserved).
- `EfiMemoryMapTests`, `IA64SalTests`, `RiscVDecoderSmokeTest` — all pass.
- `test_isa_plugin` (pre-existing IA-64 suite) aborts at an assert in
  `tests/test_isa_plugin.cpp:1551` (IA-64 state serialization). Verified
  pre-existing: the only tracked modification is the additive 30-line CMake
  block, and that binary does not link any AArch64 code. Left untouched
  per scope rules; no new regression introduced by this phase.

## 6. Limitations (explicit non-goals)

No ISO/ELF boot, no UEFI/firmware bring-up, no GIC, no MMU/translation,
no EL1 exception model, no timers/PL011/virtio, no SMP, no GUI selector,
no JIT/SIMD/FP, no load/store or system instructions, no ADDS/SUBS flag
computation, no SP-alignment checking. MVN/MOVN and the rest of A64 fault
as `UnsupportedFeature`/`UnallocatedEncoding` by design.

## 7. Proposed bounded AARCH64-2 objective

Add A64 load/store (LDR/STR immediate, unsigned offset, 32/64-bit, XZR/SP
semantics) plus `CBZ/CBNZ` and `B.cond`, reusing the same fetch/decode/
fault/bounded-run skeleton; extend the foundation suite with little-endian
memory round-trips, unaligned-access faults, and a looping countdown
program that terminates by condition (not step budget). Still no MMU,
exceptions, or devices.
