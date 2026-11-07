# PPC64 Phase 34 — EXTSW and Linux Frontier

**Result: Outcome A — PPC64-34 complete.** The general EXTSW operation passed production VM execution tests in Debug and Release, the authenticated Linux instruction retired, Linux continued beyond the Phase 33 frontier, and the next unsupported instruction is recorded. This does not claim a complete Linux boot or QEMU runtime parity.

## Repository preflight and closeout

- Workspace: `D:\dev\guideXOS_Hypervisor_PPC64`; branch: `PPC64`.
- Starting HEAD: `b4d406cd3a86607f660933a119d53d10772d2852`. The requested expected start `c7a1d90a4cca75d1d6384805b0ffc9d3f3e14e46` was not the local HEAD. The local checkout was authoritative, at `origin/PPC64` **ahead 2, behind 0** before this phase.
- Tracked and untracked worktree was clean before changes. The ignored `build/` directory was preserved.
- The initial `.phase-ppc64` was `last_completed=PPC64-33`, `next_expected=PPC64-34`. On successful closeout it becomes `PPC64-34` / `PPC64-35`.
- Shared IA-64 `.phase` was left unchanged: `last_completed=IA64-POST-MADT-6`, `next_expected=IA64-POST-MADT-7`.
- The Phase 33 report present in this checkout is `PPC64_PHASE33_SPRG1_PACA_CONTINUATION.md`. Its accepted artifact manifest was rechecked at SHA-256 `d3b91c51ffe3579bbdd7ee081c5d7cd0e48878e9c872cc632a79a508aa959bed`.
- No branch switch, reset, rebase, merge, stash, clean, cherry-pick, push, or sibling-worktree change was made.

## Instruction references and encoding

The normative reference reviewed was Power ISA 3.1C (OpenPOWER publication dated 2024-05-26). Source cross-checks used QEMU decoder and implementation commit `1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6` and Linux v7.2 opcode definitions. The QEMU decoder pattern identifies primary opcode 31 and XO 986, but represents the five unused bits as wildcards. This implementation follows the reserved-zero contract required for this profile and rejects nonzero values; it does not copy QEMU's wildcard policy.

For authenticated word `0x7CA507B4`:

| Field | Value |
|---|---:|
| Primary opcode | 31 |
| RS (source) | 5 |
| RA (destination) | 5 |
| Reserved field | 0 |
| XO | 986 |
| Rc | 0 |
| Decode | `extsw r5,r5` |

The production opcode-31 decoder recognizes EXTSW by XO 986, leaves EXTSB, EXTSH, EXTSWSLI, and other opcode-31 forms isolated, and validates the five reserved bits (`0x0000F800`). All 32 RS values, all 32 RA values, both Rc values, and every nonzero reserved-field value were exercised. A reserved-field violation is `INVALID_ENCODING`; the guest-delivery test reaches the Program vector with `SRR1[PROGILL]` set and preserves the source and destination registers.

References: [Power ISA 3.1C](https://openpowerfoundation.org/specifications/isa/), [pinned QEMU decoder](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/insn32.decode), [pinned QEMU fixed-point implementation](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate/fixedpoint-impl.c.inc), and [Linux v7.2 PPC opcode definitions](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/include/asm/ppc-opcode.h).

## Semantics and qualification

The production operation reads RS before writing RA, masks to an unsigned low word, and ORs `0xFFFFFFFF00000000` when bit 31 is set. This avoids host signed-shift and overflow behavior, uses no host endianness assumptions, and handles `RA=RS` naturally. GPR0 remains an ordinary register operand.

The independent fixed oracle includes zero, one, `0x7FFFFFFF`, `0x80000000`, all ones, mixed high and low halves, and the ten task-provided normative vectors. VM-step coverage also includes every low-word single-bit position, corresponding upper-half bits, complements, explicit alternating patterns, sign-boundary neighbors, five upper-half values sharing a negative low word, GPR0/GPR31, aliases, and distinct source/destination registers. The normative table is literal expected data; the test oracle is separate from production execution.

Rc=0 changes only RA and advances NIP by four. Rc=1 updates only CR0 from the **extended result**, with LT/GT/EQ and XER[SO] behavior; CR1–CR7 remain intact. XER (including CA, CA32, OV, OV32, SO), MSR, LR, CTR, SPRG1, PIDR, SRR0/1, DAR/DSISR, HSRR0/1, HDAR/HDSISR, unrelated GPRs, and guest RAM are preserved. No checkpoint-format change was needed.

The selected POWER9 DD2.0 64-bit profile executes EXTSW in supervisor and problem state, with SF=1 and SF=0. SF selects the address mode here; it does not disable the fixed-point facility on this 64-bit implementation. Invalid reserved encodings use the existing invalid-encoding / Program-exception path. Valid problem-state EXTSW raises no privilege fault. The pinned QEMU translator uses its `TRANS64` build-time facility macro for EXTSW; it contains no runtime SF predicate. This source check is not runtime parity evidence.

Programs A–N all pass: exact encoding; general X-form; positive and negative values; boundaries; upper-half independence; aliasing; record form; state preservation; mode qualification; decoder isolation; production `VirtualMachine::step()`; authenticated Linux retirement; and two-run continuation/frontier comparison.

## Authenticated Linux state and dataflow

The artifact verifier ran before replay. The guest was not rebuilt, patched, or modified.

| Artifact | SHA-256 |
|---|---|
| Kernel `vmlinuz-7.2.14+deb14-powerpc64` | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Kernel configuration | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| FDT | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |

The artifact verifier confirmed an ELF64 big-endian PPC64 ET_EXEC kernel with entry `0xC000000000000000` and the accepted single load segment. Replay retained POWER9 DD2.0 PVR `0x004E1200`, LPID 1, guest-real translation, the RTAS-disabled probe configuration, and the authenticated FDT. Both replay lifecycles started from the authenticated kernel entry and reproduced the accepted PPC64-33 path, including the 231,806 relocation records, real `boot_paca.5` installation in SPRG1, and the 2,641,070-retirement prefix to the SPRG1 write. The Phase 33 boundary state and memory evidence matched across independent VMs; no checkpoint was used to skip the prefix. The full Phase 33 replay tests also passed.

The reproduced SPRG1 write is at NIP `0xC000000000034D48` (`mtspr 273,r3`): R3 and R13 are both `0xC000000001F42380`, SPRG1 changes from zero to `0xC000000001F42380`, and NIP advances to `0xC000000000034D4C`.

At the EXTSW frontier:

| State | Value |
|---|---|
| NIP / raw word | `0xC00000000138A414` / `0x7CA507B4` |
| R5 before / low word | `0x0000000000000001` / `0x00000001` |
| Sign / independent result | nonnegative / `0x0000000000000001` |
| R5 after | `0x0000000000000001` |
| CR / XER before and after | `0x24000000` / `0x20040000` (unchanged) |
| MSR / LPID | `0x8000000000000002` / 1 |
| SPRG1 | `0xC000000001F42380` (unchanged) |
| Post-instruction NIP | `0xC00000000138A418` |
| Guest RAM fingerprint | unchanged by EXTSW |

ELF64 big-endian PPC64 ET_EXEC inspection places the instruction at `fdt_path_offset+0x44`; the function starts at `0xC00000000138A3D0` and has size `0x50`. Its `bl strlen`, `or r5,r3,r3`, `extsw r5,r5`, and tail branch to `fdt_path_offset_namelen` establish that R5 is the path-length argument. The authentic caller is `early_init_dt_scan_root` at call site `0xC000000001F22568`. The matching Linux source location is `drivers/of/fdt.c::early_init_dt_scan_root()`, which calls `fdt_path_offset(initial_boot_params, "/")`; the libfdt wrapper obtains the one-character root-path length with `strlen` and passes it as the signed `int namelen` argument. Thus this R5 is a **count** equal to 1, not an index, offset, or branch-condition input. The ELF gives symbol and instruction-level attribution; its build does not embed a Debian source-package revision, so no exact Debian package commit is claimed.

Source references: [Linux v7.2 early FDT initialization](https://github.com/torvalds/linux/blob/v7.2/drivers/of/fdt.c#L3775) and [Linux v7.2 libfdt path routines](https://github.com/torvalds/linux/blob/v7.2/scripts/dtc/libfdt/fdt_ro.c#L237). The routine belongs to early device-tree initialization, not `setup_paca()`.

## Continuation, frontier, and repeatability

EXTSW retired through the production VM step. Linux executed 242 more instructions after the old Phase 33 total of 2,651,354, reaching **2,651,596 total retirements**. The last successful NIP is `0xC00000000138848C`. At `0xC000000001388490`, fetch translation and execute permission both succeeded; the first unsupported word is `0x7BA91788`. Independent field decode: primary opcode 30, RS=29, RA=9, MD XO=2, Rc=0. ELF attribution is `fdt_next_tag+0x70`. The stop is classified as an unsupported-instruction frontier, not a Linux failure. No instruction beyond EXTSW was implemented in this phase, and full Linux boot is not claimed.

Two independent production VM lifecycles agree on kernel/FDT identity, EXTSW pre/post R5, CR/XER/MSR/SPRG1, post-EXTSW NIP, final frontier, continuation trace digest `0x57164871970afdf8`, final CPU registers, and guest RAM fingerprint. The compact digest covers the Phase 33 continuation from the SPRG1 write onward; the earlier 2,641,070-instruction prefix was replayed from entry in both VMs and compared at its architectural frontier by retirement count, state, and memory evidence.

## Tests, checkpoints, and regressions

- Final Phase 34 executable: **13,119 checks**, comprising **13,046 focused**, **71 Linux replay**, and 2 setup/artifact checks; zero failures. Debug and Release runs report the same counts and identical artifact-manifest hashes.
- Final full CTest: **49 registered, 43 passed, 0 executed failures, 6 Not Run**, exit code 8. The Not Run entries are `test_syscall_dispatcher`, `test_debugger`, `test_profiler`, `test_fuzzer`, `test_vm_isolation`, and `test_elf_validation`; their executables are absent. They are not counted as passes.
- IA-64 SAL passed within CTest. Standalone IA-64 ACPI passed all seven named checks; IA-64 interrupt controller and Processor Interrupt Block tests both passed.
- VM checkpoint and Phase 33 SPRG1 tests passed. CPU-state version 6 and the v1–v5 readers remain qualified by Phase 33; EXTSW changed no checkpoint fields.
- Current CTest output totals **479,228 Phase 1–33 checks**: Phases 1–30 = 471,029; Phase 31 = 6,507 plus its separate 121-check Linux replay; refreshed Phase 32 = 614; current Phase 33 = 957. This is 7 above the task's nominal 479,221, so the observed per-test output totals are reported rather than forcing the historical figure. Phase 34 adds 13,119, for 492,347 checks across Phases 1–34.
- `git diff --check` passed. QEMU runtime remains `QEMU_REFERENCE_PENDING`; QEMU/QAC was not run.

## Artifacts and hashes

Generated artifacts remain outside Git:

- Debug/authentic Linux run: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase34\run-1794087596911`
- Release/authentic Linux run: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase34\release-run-final-state`
- Both `artifact-sha256-manifest.txt` files: SHA-256 `f376e2639cc9c6f56dab33b7a1cfc0ad1eac59a1a1d35f08bc5148caf180c0ea` (18 entries each, zero mismatches).
- Regression, full CTest, and standalone IA-64 evidence is under the Debug run's `regressions` directory. Its supplemental SHA-256 manifest is `9b600d867fb4c5fd3a3b8ff82f79667842f7694686518c42eb33644bc489c7e5` (9 entries, zero mismatches).
- The authenticated Phase 33 artifact directory and manifest were preserved unchanged.

## PPC64-35 recommendation

Resume from the reproducible `fdt_next_tag+0x70` frontier at `0xC000000001388490` (`0x7BA91788`, opcode 30, MD XO 2). Independently identify that exact form and its architectural mode/reserved-field requirements before considering implementation. Keep the accepted PPC64-34 two-run EXTSW/Linux trace and all authenticated inputs as the baseline. Do not infer a full boot milestone from this bounded continuation.
