# PPC64 Phase 3 — Integer Loads/Stores, Memory Faults, Exception Foundation

Branch: `PPC64`. Phase marker: `.phase-ppc64` (`last_completed=PPC64-3`,
`next_expected=PPC64-4`; the shared IA-64 `.phase` file is untouched).

Primary semantic reference: OpenPOWER Power ISA v3.1C, Book I (integer
storage-access facility) and Book III (exception model, deferred). This
document claims only the subset below — **not** complete Power ISA
compatibility and **not** Book III exception delivery.

## 1. Exact new instruction coverage

All encodings are big-endian 32-bit, executed through the production
`PPC64ISAPlugin` (decode + execute + `step`) against real `Memory`.

| Mnemonic | Format | Encoding | Semantics (64-bit) |
|---|---|---|---|
| `lbz RT,D(RA)` | D | 34 | `RT := zeroext(MEM[EA,1])` |
| `lbzu RT,D(RA)` | D | 35 | as `lbz`, then `RA := EA` |
| `lhz RT,D(RA)` | D | 40 | `RT := zeroext(MEM[EA,2])` |
| `lhzu RT,D(RA)` | D | 41 | as `lhz`, then `RA := EA` |
| `lha RT,D(RA)` | D | 42 | `RT := signext16(MEM[EA,2])` |
| `lhau RT,D(RA)` | D | 43 | as `lha`, then `RA := EA` |
| `lwz RT,D(RA)` | D | 32 | `RT := zeroext(MEM[EA,4])` |
| `lwzu RT,D(RA)` | D | 33 | as `lwz`, then `RA := EA` |
| `lwa RT,DS(RA)` | DS | 58, XO 2 | `RT := signext32(MEM[EA,4])`; EA%4==0 required |
| `ld RT,DS(RA)` | DS | 58, XO 0 | `RT := MEM[EA,8]` |
| `ldu RT,DS(RA)` | DS | 58, XO 1 | as `ld`, then `RA := EA` |
| `stb RS,D(RA)` | D | 38 | `MEM[EA,1] := (RS)[56:63]` |
| `stbu RS,D(RA)` | D | 39 | as `stb`, then `RA := EA` |
| `sth RS,D(RA)` | D | 44 | `MEM[EA,2] := (RS)[48:63]` |
| `sthu RS,D(RA)` | D | 45 | as `sth`, then `RA := EA` |
| `stw RS,D(RA)` | D | 36 | `MEM[EA,4] := (RS)[32:63]` |
| `stwu RS,D(RA)` | D | 37 | as `stw`, then `RA := EA` |
| `std RS,DS(RA)` | DS | 62, XO 0 | `MEM[EA,8] := (RS)` |
| `stdu RS,DS(RA)` | DS | 62, XO 1 | as `std`, then `RA := EA` |
| `lbzx RT,RA,RB` | X | 31, XO 87 | `RT := zeroext(MEM[EA,1])` |
| `lhzx RT,RA,RB` | X | 31, XO 279 | `RT := zeroext(MEM[EA,2])` |
| `lhax RT,RA,RB` | X | 31, XO 343 | `RT := signext16(MEM[EA,2])` |
| `lwzx RT,RA,RB` | X | 31, XO 23 | `RT := zeroext(MEM[EA,4])` |
| `lwax RT,RA,RB` | X | 31, XO 341 | `RT := signext32(MEM[EA,4])`; EA%4==0 required |
| `ldx RT,RA,RB` | X | 31, XO 21 | `RT := MEM[EA,8]` |
| `stbx RS,RA,RB` | X | 31, XO 215 | `MEM[EA,1] := (RS)[56:63]` |
| `sthx RS,RA,RB` | X | 31, XO 407 | `MEM[EA,2] := (RS)[48:63]` |
| `stwx RS,RA,RB` | X | 31, XO 151 | `MEM[EA,4] := (RS)[32:63]` |
| `stdx RS,RA,RB` | X | 31, XO 149 | `MEM[EA,8] := (RS)` |
| `sc` | SC | 17, `0x44000002` | system-call trap surface (no dispatch) |

Anchor cross-checks asserted in tests: `lbz r3,0(r4)=0x88640000`,
`lwz r3,4(r4)=0x80640004`, `stb r3,0(r4)=0x98640000`,
`stw r3,0(r4)=0x90640000`, `lhz r3,0(r4)=0xA0640000`,
`lha r3,0(r4)=0xA8640000`, `sth r3,0(r4)=0xB0640000`,
`ld r3,0(r4)=0xE8640000`, `lwa r3,0(r4)=0xE8640002`,
`std r3,0(r4)=0xF8640000`, `stdu r1,-32(r1)=0xF821FFE1`,
`lbzx r3,r4,r5=0x7C6428AE`, `ldx r3,r4,r5=0x7C64282A`,
`sc=0x44000002`.

### Supported and invalid forms

- D-form update loads (`lbzu`/`lhzu`/`lhau`/`lwzu`) are invalid when
  `RA==0` or `RA==RT` (Power ISA Book I §4.6); D/DS-form update stores
  (`stbu`/`sthu`/`stwu`/`stdu`) are invalid only when `RA==0`.
  Store-with-update `RS==RA` overlap is well-defined and supported: the
  pre-update source value is stored, then `RA := EA` (verified:
  `stdu r5,-8(r5)` and `stwu r6,4(r6)`).
- `ldu` is invalid when `RA==0` or `RA==RT`; `stdu` is invalid when
  `RA==0`.
- DS-form XO=3 is reserved on both opcodes 58 and 62; XO=2 is reserved
  on opcode 62. All decode as unsupported → `EXCEPTION`.
- Indexed X-forms require Rc=0; Rc=1 decodes as unsupported.
- Update-indexed XO values (`lbzux=119`, `lhzux=311`, `lhaux=375`,
  `lwzux=55`, `lwaux=373`, `ldux=53`, `stbux=247`, `sthux=439`,
  `stwux=183`, `stdux=181`) decode as unsupported → `EXCEPTION`
  (classified `INVALID_INSTRUCTION`; deferred, see §9).
- `sc` accepts exactly `0x44000002` (LEV=0, all reserved bits zero).
  Any other primary-opcode-17 word (LEV≠0, reserved bits set, missing
  encoding bit) is rejected as `INVALID_INSTRUCTION`.
- Loads/stores never mutate CR, XER, LR, or CTR (asserted with sentinel
  `CR=0x12345678`, `XER=CA`, `LR`, `CTR` values across mixed sequences).

## 2. Effective-address rules

- D-form: `EA := (RA==0) ? EXTS(D) : (RA) + EXTS(D)`, with the 16-bit `D`
  sign-extended. `RA=0` selects the constant zero even when GPR0 holds
  garbage (tested); GPR0 itself is never modified by the rule.
- DS-form: `EA := (RA==0) ? EXTS(DS||0b00) : (RA) + EXTS(DS||0b00)`,
  with the 14-bit `DS` sign-extended then shifted (offsets are always
  multiples of 4, range −32768…+32764; max-displacement decode to
  EA=32764 is asserted).
- X-form: `EA := ((RA==0) ? 0 : (RA)) + (RB)`. `RB=0` reads GPR0 as an
  ordinary index register (no zero rule on RB — asserted).
- All addition is unsigned 64-bit wraparound (mod 2⁶⁴, no host UB):
  `EA = 0xFFFF…FF + 1` yields `0x0` and a load there succeeds reading
  guest address zero (asserted); negative D/DS displacements and
  negative index registers wrap identically.

## 3. Data endianness

Big-endian throughout: the first byte at EA is the most significant.
Loads assemble `value := (value << 8) | byte[i]` for `i` in `0..width`;
stores decompose MSB-first. Verified byte patterns include
`stw` of `0x9ABCDEF5` → `9A BC DE F5`, `std` of
`0x0123456789ABCDEF` → `01 23 45 67 89 AB CD EF`, and Program A (§7).

## 4. Fault classification (`PPC64Fault`, per CPU instance)

| Category | Access | Width | When |
|---|---|---|---|
| `INVALID_INSTRUCTION` | NONE | 0 | unknown opcode/XO, reserved DS XO, Rc on indexed, bad `sc` shape, stale decode |
| `INVALID_ENCODING` | NONE | 0 | known form, illegal fields (bad update registers) |
| `FETCH_FAILURE` | FETCH | 4 | `IMemory::Read` of the instruction word threw |
| `DATA_LOAD_FAILURE` | READ | 1/2/4/8 | data `Read` threw, or `[EA,EA+width)` wraps past 2⁶⁴−1 |
| `DATA_STORE_FAILURE` | WRITE | 1/2/4/8 | data `Write` threw, or range wraps |
| `ALIGNMENT` | FETCH / READ | 4 | misaligned NIP; `lwa`/`lwax` with EA%4≠0 |
| `SYSCALL` | NONE | 0 | `sc` trap (returns `ISAExecutionResult::SYSCALL`) |

Each record carries: faulting-instruction address (`cia`), raw encoding
when fetched (`hasRaw`), effective address when an access was attempted
(`hasEA`), access type, width, category, host-side diagnostic text
(`reason`, e.g. the `std::out_of_range` / `PageFault` message), and
`resumable` (false only for encodings that can never execute:
`INVALID_INSTRUCTION`/`INVALID_ENCODING`; true for fetch/data/
alignment/syscall, where fixing the environment makes retry meaningful).

**Distinction preserved:** a host `IMemory` failure is recorded as
observed (`DATA_LOAD_FAILURE` etc. with the host message) and is
explicitly **not** claimed to be a guest DSI/ISI. Categories name the
emulator-observed condition and reserve space for later Book III
DSI/ISI/Alignment/Program-interrupt delivery.

## 5. Alignment semantics

- Instruction fetch requires `NIP % 4 == 0`, else `ALIGNMENT`/FETCH
  (pre-existing Phase 1 behavior, now recorded).
- Ordinary byte/halfword/word/doubleword loads/stores permit **any** EA:
  Power ISA v3.1 does not mandate Alignment interrupts for naturally
  unaligned ordinary fixed-point accesses, so imposing one would be an
  artifact of host page organization. Unaligned `lhz`/`lwz`/`ld`/`stw`
  at odd EAs are asserted working (e.g. `ld` at `0x401`).
- `lwa`/`lwax` load a 32-bit word and architecturally require a
  word-aligned EA: EA%4≠0 raises `ALIGNMENT`/READ with the EA recorded
  and no register touched (asserted for both D/DS and indexed paths).
- Range wrap (`[EA,EA+width)` past 2⁶⁴−1, e.g. `lhz` at
  `EA=0xFFFF…FF`) fails deterministically before any host call.
- OOB and cross-boundary accesses (EA valid but `EA+width` past the end,
  e.g. 4-byte store at `size−2`) raise host bounds errors → data-failure
  records; guest memory and registers are preserved (asserted by
  64-byte snapshot comparison).

## 6. Exception precision and limitations

For every fault: NIP is preserved (never advanced), no partial
destination/base update is committed (destination written only after a
successful read; RA updated only after success; store source captured
before any RA update), the existing `IISA` result is returned
(`EXCEPTION`, or `SYSCALL` for `sc`), instances keep separate records,
and `reset()` clears fault state. The record is **sticky**: later
successful execution retains it until `clearFault()`/`reset()`
(asserted). The record is host-side diagnostics only and is **never**
serialized — the 304-byte Phase 1 checkpoint format
(magic/version/GPRs/NIP/LR/CTR/CR/XER/MSR) is byte-identical, and
hand-built Phase 1 blobs still load.

Limitations (not claimed): no SRR0/SRR1, no MSR privilege transitions,
no exception-vector delivery, no guest DSI/ISI synthesis from host
faults, no atomic (load-reserve/store-conditional) or
multiple/string/vector forms.

## 7. Memory transaction guarantees (measured `IMemory` contract)

- Valid-range checking: YES — `Memory::Read/Write` bounds-check the
  **full** `[address, address+size)` range (`checkBounds`) and throw
  `std::out_of_range` **before** any byte is copied: multi-byte stores
  are all-or-nothing for bounds errors, so no rollback mechanism was
  needed (asserted by memory snapshot comparison on failed stores).
- Permission checking: PARTIAL — the MMU checks only the **start**
  page (`CheckPermissionOrThrow(address,…)`); a range crossing into a
  differently-permissioned page is not re-checked mid-range. Documented
  as a `Memory`-implementation limitation, not worked around.
- Read-only regions: YES via MMU page permissions — store to a
  `READ`-only page raises `PageFault(PERMISSION_WRITE)` → recorded as
  `DATA_STORE_FAILURE` (asserted); loads from it still succeed.
  Write-only (`WRITE`) pages symmetrically fault loads (asserted).
- Unmapped pages: access starting in an unmapped page raises
  `PageFault(NOT_MAPPED)` → `DATA_LOAD_FAILURE` with the host
  diagnostic preserved (asserted).
- MMIO handlers: supported transparently — `Memory` consults registered
  `IMemoryMappedDevice` ranges before flat/RAM access, so the PPC64
  single-call `Read`/`Write` path works through MMIO without changes.
- Hook-denied accesses surface as `std::runtime_error` → recorded as
  data failures like any other host rejection.
- Shared-memory semantics untouched: no change to `IMemory`, `Memory`,
  or MMU code; IA-64 paths are unaffected (BasicTests pass, §8).

## 8. Executable programs (all genuine PowerPC bytes via the production decoder)

- **A — Memory round trip** (16 steps): `stb`/`sth`/`stw`/`std` of
  `0x5A`, `0x1234`, `0x89ABCDEF`, `0xFFFFFFFFFFFF3344` then
  `lbz`/`lhz`/`lwz`/`ld` back. Registers, 16-byte BE layout
  (`5A AA 12 34 89 AB CD EF FF FF FF FF FF FF 33 44` — note the
  untouched `0xAA` gap at `0x401`, proving no byte bleed), and both
  guard regions verified.
- **B — Signed data** (20 steps): `lha`→`0xFFFF…FFFF` vs
  `lhz`→`0xFFFF` on `0xFFFF`; `lha`→`0xFFFF…8000` vs `lhz`→`0x8000` on
  `0x8000`; `lbz`→`0xFF`; `lwa`→`0xFFFF…FFFF` vs `lwz`→`0xFFFFFFFF`;
  `lwa`→`0xFFFF…80000000` vs `lwz`→`0x80000000`. All 64-bit results
  asserted.
- **C — Stack/subroutine** (12 steps): `bl` to a function using
  `stdu r1,-32(r1)` framing, `mflr`/`std` LR save, body, `ld`/`mtlr`
  restore, `addi` teardown, `blr`. Final `r3=15`, `r1` restored to
  `0x800`, `LR=0x10C`, frame bytes (`[0x7E0]`=saved SP,
  `[0x7F8]`=saved LR) verified — including the store-with-update
  `RS==RA` overlap rule in production.
- **D — Loop with memory** (36 steps): five `lwzx` iterations over
  `[3,1,4,1,5]` with Phase 2 `add`/`cmpwi`/`bne`. Accumulator `r3=14`,
  count `r5=0`, byte offset `r6=20`, array unchanged, sum spilled via
  `std` as BE `…0E`, NIP at end. Step count `4+5*6+2` asserted.
- **E — Fault injection**: success prefix (`addi`/`stb`/`lbz`), then
  OOB `lwz` (EA=`0x1000`): `EXCEPTION`, NIP held at `0x110`,
  destination sentinel intact, `DATA_LOAD_FAILURE`/READ/width-4/EA
  recorded; then OOB fetch at `0x5000`: `FETCH_FAILURE`/FETCH; then
  `sc`: `SYSCALL`, NIP held at `0x200`, `SYSCALL`/NONE recorded.

## 9. Test results

`tests/test_ppc64_phase3.cpp` (registered as `test_ppc64_phase3` +
`PPC64Phase3Tests` in CMake/CTest): **535/535 checks pass**.
Phase 1: **148/148** maintained. Phase 2: **399/399** maintained.
Shared/IA-64 CTest run: 9/9 runnable suites pass (`BasicTests`,
`NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`,
`VMCheckpointTests`, `RiscVDecoderSmokeTest`, PPC64 1/2/3). Six suites
report `Not Run` (`SyscallTests`, `DebuggerTests`, `ProfilerTests`,
`FuzzerTests`, `VMIsolationTests`, `ELFValidationTests`) because their
binaries cannot link under this MinGW toolchain (`undefined reference
to __imp_WSAGetLastError` — missing `ws2_32` on those targets) and the
legacy `test_isa_plugin` NaT serialization abort — all pre-existing,
listed as known, and unrelated to this phase (no shared file was
modified; link errors reference `RestAPIServer.cpp`/`WebUIServer.cpp`
only). No new failures introduced.

## 10. Phase 1/2 compatibility and remaining gaps

- Preserved: BE fetch, fixed-width decode, `addi`/`addis` zero-base,
  all Phase 2 arithmetic/CR/XER/branch/SPR behavior, misaligned/stale/
  OOB fetch handling (now additionally fault-recorded), 304-byte
  snapshots, per-instance isolation, disassembly of old mnemonics.
- `getVersion()` is now `"3.0-phase3"` (informational only; the
  checkpoint magic/version bytes are unchanged).
- Remaining ISA gaps: update-indexed forms (`lbzux`…`stdux`),
  load-reserve/store-conditional, multiple/string, byte-reverse,
  floating-point/vector, privileged SPRs and Book III state —
  recommended **PPC64-4** direction: Book III exception delivery
  (SRR0/SRR1, DSI/ISI vectors) on top of this fault foundation, then
  atomics, guided by the recorded `PPC64Fault` fields which already
  carry everything delivery needs (CIA, EA, access, width, category).
