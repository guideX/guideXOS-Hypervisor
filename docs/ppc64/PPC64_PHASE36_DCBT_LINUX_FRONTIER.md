# PPC64 Phase 36 — DCBT Cache Hint and Linux Frontier

## Result

Phase 36 adds ordinary PPC64 `dcbt` to the POWER9 functional profile. It is decoded as an optional cache hint and retires without cache simulation, operand translation, a guest memory access, or a guest-visible register effect. The authentic Debian PPC64BE Linux instruction at `0xC000000000170424` retired, and two fresh replays reached the same next unsupported instruction.

All Phase 36 acceptance gates passed. The phase marker is advanced to PPC64-36 in the closeout. QEMU runtime was not used; the task's QAC restriction remains in force.

## Starting state and safety

- Checkout: `D:\dev\guideXOS_Hypervisor_PPC64`.
- Branch: `PPC64`.
- Starting HEAD: `9239494cafdb0f0fa65a312e38736fd3b2e1ba72`.
- The starting branch was one commit ahead of `origin/PPC64`, with zero commits behind.
- Tracked worktree was clean at start. The existing ignored `build/` was preserved.
- `.phase-ppc64` was at `last_completed=PPC64-35`, `next_expected=PPC64-36`; the shared IA-64 `.phase` was not changed.
- The Phase 35 report and its accepted artifact directories were present. The PPC64 test inventory contained six historical missing executables, recorded with the CTest results below.
- No branch switching, reset, rebase, merge, stash, clean, cherry-pick, push, sibling-worktree edit, kernel patch, or FDT patch was performed.

## DCBT encoding and selected profile

The authentic instruction word is `0x7C001A2C` at effective NIP `0xC000000000170424`:

| Field | Value |
|---|---:|
| Primary opcode | 31 |
| TH | 0 |
| RA | 0 |
| RB | 3 |
| XO | 278 |
| Rc / low reserved bit | 0 |

The ISA decoder now recognizes only opcode 31/XO 278 as `DecodedKind::DCBT`. TH is stored separately from RT, so it cannot be mistaken for a destination GPR. `hasFeature("dcbt")` reports support in the selected POWER9 profile. Rc=1 and TH values outside the selected profile's qualified forms are invalid encodings. Neighboring cache XOs remain unsupported.

The POWER9 profile accepts TH=0, TH=1 through 8, and TH=10, 11, 16, and 17. POWER9 specifies TH=1 through 7 to behave like TH=0; the remaining accepted values have POWER9-defined stream/descriptor/transient meanings. TH=9, 12, 15, 18, 30, and 31, plus Rc=1, are rejected by focused tests. TH does not change modeled register or memory effects.

DCBT computes the indexed effective address as `(RA == 0 ? 0 : GPR[RA]) + GPR[RB]`. This uses the complete GPR values; for SF=0, the low 32 bits are the storage-address portion. The hint implementation does not consume or dereference that address. Focused tests independently cover RA=0 despite nonzero GPR0, nonzero RA, RB=0, 64-bit wraparound, cache-block boundaries, and SF=0/SF=1 address-width cases.

The selected profile is POWER9, PVR `0x004E1200`. The OpenPOWER ISA is the normative architecture reference. IBM's POWER9 manual describes DCBT as a cache/translation hint, says translation-related conditions do not cause ordinary interrupts (apart from trace interrupts), documents TH=1..7 as TH=0 behavior, and describes optional cache/ERAT/TLB and reference-bit effects. This emulator has no cache hierarchy or translation-cache model, so its explicit functional policy is to complete ordinary DCBT with no modeled cache action, no data translation, and no page-table mutation. It does not fabricate a load, store, cache timing, or data-storage exception. This policy is scoped to DCBT and does not classify other cache instructions as no-ops.

References: [OpenPOWER ISA 3.1C](https://openpowerfoundation.org/specifications/isa/), [IBM POWER9 User's Manual, §§4.6.2.5–4.6.2.12](https://wiki.raptorcs.com/w/images/c/ce/POWER9_um_OpenPOWER_v21_10OCT2019_pub.pdf), [IBM DCBT reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-dcbt-data-cache-block-touch-instruction), [pinned QEMU decoder](https://raw.githubusercontent.com/qemu/qemu/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/insn32.decode), [pinned QEMU translator](https://raw.githubusercontent.com/qemu/qemu/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate.c), and [Linux v7.2 `prefetch()` helper](https://raw.githubusercontent.com/torvalds/linux/v7.2/arch/powerpc/include/asm/processor.h).

QEMU's pinned decoder uses an X-form TH/RA/RB layout with XO 278 and a reserved low bit; its translator checks the cache feature and emits no explicit memory operation. Linux v7.2's helper emits `dcbt 0,%0` after a null check. These are software references only; no QEMU runtime equivalence is claimed.

## Focused tests and integrity

`tests/test_ppc64_phase36.cpp` is registered as `PPC64Phase36Tests`. It runs each valid instruction word through `VirtualMachine::step()` and covers programs A–N:

- **A–C:** exact Linux word, field decode, TH/RA/RB variants, RA=0 base rule, addition, wraparound, RB=0, and SF=0/1 storage-address width.
- **D–F:** GPR, CR, XER, MSR, LR, CTR, SPRG1, interrupt, and RAM preservation; unmapped and out-of-range targets; cache-boundary cases; MMIO callbacks; and a protected RTAS region.
- **G–I:** POWER9 TH forms, invalid TH/Rc, supervisor and problem state, translated effective-address setup, and continued rejection of DCBTST, DCBZ, DCBF, DCBST, and ICBI.
- **J–L:** separate VM state, LPID 1, checkpoint state-size/version compatibility, and production VM stepping.
- **M–N:** authentic Linux DCBT retirement, bounded continuation, two fresh replays, and comparison of the resulting frontier, traces, CPU state, and RAM.

Debug Phase 36 reported 1,315 total `CHECK` assertions, including 1,313 focused checks; the 58 Linux replay checks are included in those focused checks. The two remaining assertions verify evidence-file writes. Failures: zero. Release Phase 36 reported the same counts and zero failures.

Checkpoint state remains version 6 and no CPU-state or checkpoint field was added. The test checks state sizes and the existing checkpoint regression target is included in the shared suite. The focused LPID test keeps a separate VM's complete register state unchanged while another VM retires DCBT.

## Authenticated Linux replay

The artifact verifier passed for the original, unmodified guest:

- Kernel SHA-256: `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd`.
- Configuration SHA-256: `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa`.
- FDT SHA-256: `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37`.
- CPU PVR: `0x004E1200`; LPID: 1; original ELF placement, entry registers, partition radix context, guest-real mode, SPRG1/PACA state, RTAS-disabled probe, and memory protections were preserved.

The replay reproduced the Phase 35 prefix and its 2,652,206-retirement endpoint before executing DCBT. The live pre-instruction snapshot was:

```text
NIP       0xC000000000170424
word      0x7C001A2C
TH/RA/RB  0/0/3
R3        0xC000000007FF02D8
EA        0xC000000007FF02D8
MSR       0x8000000000000002
CR        0x24000040
XER       0x20040000
SPRG1     0xC000000001F42380
LPID      1
```

The genuine production fetch decoded as `dcbt 0,r3,0`; it retired once, changed NIP to `0xC000000000170428`, advanced cycles/timebase from 2,652,206 to 2,652,207, and preserved RAM and the original FDT bytes. The complete bounded state and the preceding instruction window are in `authentic-dcbt-state.txt`.

ELF inspection resolves no enclosing symbol or offset for address `0xC000000000170424`. The bounded call path reaches the helper from `early_init_dt_scan_root` at call site `0xC000000001F22568`; its source expression is `fdt_path_offset(initial_boot_params, "/")`. The exact machine instruction is not attributed to Linux's generic `prefetch()` helper, and no later use of R3 is claimed beyond the captured execution evidence.

After DCBT, Linux retired 2,705 more instructions, for 2,654,911 total. Last successful NIP: `0xC000000000170880`. Next attempted NIP: `0xC000000000170884`, word `0x7D293036`, independently decoded as unsupported opcode 31. Stop class: unsupported/invalid instruction. This is bounded progress, not a Linux boot claim. The new frontier's ELF inspection does not resolve a symbol. The replay records the prefix and registers; Phase 35's FDT validation evidence remains 921 bytes and 35 tags.

Two independent fresh VM replays matched kernel/FDT hashes, DCBT R3 and EA, post-DCBT NIP, total retired count, next frontier, trace digest, final CPU state, final RAM fingerprint, and DCBT FDT preservation. `two-run-comparison.txt` records equal traces (`0x5bb063d3d1e90975`), equal final CPU and RAM state, and the common frontier `0xC000000000170884 / 0x7D293036`.

## Regression results

All available PPC64 phase executables (Phases 1–15, 17–36; Phase 16 shares the Phase 15 target) and the requested shared targets rebuilt successfully in Debug. Full Debug CTest had 51 entries: 45 passed, six historical executables were Not Run, and there were zero executed failures. The missing executables are `test_syscall_dispatcher`, `test_debugger`, `test_profiler`, `test_fuzzer`, `test_vm_isolation`, and `test_elf_validation`. The PPC64 Phase 31 Linux replay, Phases 32–36, VM checkpoint, and IA-64 SAL tests all passed. Standalone IA-64 ACPI, interrupt-controller, and processor-interrupt-block tests also passed with exit code 0.

The first Debug CTest pass surfaced stale Phase 31, 32, and 35 assertions that expected DCBT to remain unsupported. Those historical replay suites now stop explicitly at the pre-DCBT Phase 35 boundary; Phase 36 separately retires the real instruction and continues Linux. The final full Debug CTest run had no executed failures. Release CTest ran Phase 35 and Phase 36 and passed both. Phase 35 Release reported 797,260 total checks, 797,175 focused, and 83 replay checks; Phase 36 Release reported 1,315 total, 1,313 focused, and 58 replay checks. The separate Debug and Release suites are not added together.

The unrelated all-target Windows socket-link issue in `test_mov_imm` was not expanded into this change. The targeted Phase 36 and PPC64/shared test builds succeeded.

Prior PPC64 report totals were 492,347 checks for Phases 1–34 and 797,261 Debug checks for Phase 35, 1,289,608 reported through Phase 35. Adding Phase 36's 1,315 Debug `CHECK` assertions gives 1,290,923 reported Debug checks. The 58 Phase 36 Linux replay checks are a subset of its 1,313 focused checks. Release counts are validation results and are not added again to this aggregate.

## Artifacts

Debug replay artifacts are in `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase36\debug-run-final`. The artifact SHA-256 manifest is `artifact-sha256-manifest.txt`. Key hashes:

| Artifact | SHA-256 |
|---|---|
| `authentic-dcbt-state.txt` | `d438c95b7a7a048446e611a1bd0d1af871843e340bbbaff6d5db2446c269d3da` |
| `bounded-linux-continuation.txt` | `da24ba5e5150e763ce05fbd8cb3d5614a614b2c165c76d5893218662a41faefb` |
| `two-run-comparison.txt` | `c6907290c5e1e5a9feaa2fc3791960dfbb1ac9a253fafc9f605d46687a81ceaf` |
| `focused-checks.txt` | `a2283112f2b20120ec4a038b1a3bce7cb195b5ab014c0311086bb4d30818acb3` |
| `next-frontier-elf-inspection.txt` | `31a03983f433d9990f0b158dc59e0d6fe1f10affa96a9e6165ad4f76b441d9df` |

The Release Phase 36 replay is in `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase36\release-run-final`; Release Phase 35 regression output is in `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase35\release-run-phase36-regression`. Build and regression logs, summaries, and their SHA-256 manifest are in `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase36\regression-final`; the manifest SHA-256 is `a1781f4309d510af494947c066de4967c6faba07feb8841d4186fd5d69beb637`.

All large traces and kernel artifacts remain outside Git. The accepted Phase 35 artifact manifests were left intact.

## Outcome and Phase 37 recommendation

**Outcome A — PPC64-36 complete.** The DCBT encoding, selected POWER9 TH forms, no-effect safety policy, authentic Linux retirement, deterministic replay, bounded next frontier, Debug/Release regressions, and shared IA-64 checks passed. `.phase-ppc64` is advanced to PPC64-36, and the validated work is committed locally.

Recommended PPC64-37 work after closeout: investigate `0x7D293036` at `0xC000000000170884` independently, beginning with its exact encoding and enclosing binary control flow. Do not implement it from this Phase 36 report alone.
