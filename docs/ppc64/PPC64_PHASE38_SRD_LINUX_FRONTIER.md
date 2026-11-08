# PPC64 Phase 38 — SRD and Linux Frontier

## Result

**Outcome A — PPC64-38 complete.** The PPC64 ISA plugin recognizes and executes general X-form SRD (XO 539), including the complete seven-bit shift count, unsigned logical zero fill, aliases, record form, and POWER9 mode availability. SRD retired through production `VirtualMachine::step()` in two fresh runs of the unchanged Debian Linux guest. Both runs continued to the same next unsupported instruction.

QEMU runtime was not used. Runtime status remains `QEMU_REFERENCE_PENDING`.

## Starting repository and closeout state

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`.
- Branch: `PPC64`.
- Starting HEAD: `acb5ae2d3152246b901aed212148b16acff33faf` (`Add PPC64 SLD and Linux continuation`).
- Starting upstream: `origin/PPC64` at `5d5891c6b69fb4160e87217a7d5951f967b93be1`; the branch was one commit ahead and zero behind. No fetch or push was performed.
- Starting tracked worktree was clean. The existing ignored `build/`, `Testing/`, and `cmake-build-phase36-release/` directories were preserved.
- Shared IA-64 `.phase` was not changed.
- Starting `.phase-ppc64`: `last_completed=PPC64-37`, `next_expected=PPC64-38`.
- Final `.phase-ppc64`: `last_completed=PPC64-38`, `next_expected=PPC64-39`.

## Instruction, decoder, and semantics

The authenticated word `0x7D293436` at `0xC000000000170888` independently decodes as:

| Field | Value |
|---|---:|
| Primary opcode | 31 |
| RS | 9 |
| RA | 9 |
| RB | 6 |
| XO | 539 |
| Rc | 0 |

Mnemonic: `srd r9,r9,r6`.

SRD is separately represented from SLD (XO 27), SRW, SLW, SRAD, SRADI, RLDICL, RLDICR, RLDIC, and RLDIMI. The plugin snapshots the original source and count before writing RA, uses unsigned 64-bit arithmetic, masks RB to its low seven bits, shifts only for counts below 64, and returns zero for counts 64–127. No host shift by 64 or more occurs. The source's sign bit is not extended.

For Rc=0, CR and XER are preserved. For Rc=1, the existing CR0 updater receives the final SRD result, sets LT/GT/EQ from the result, copies XER[SO], and preserves CR1–CR7. SRD never modifies XER. It accesses no memory and does not change architectural state other than RA, optional CR0, and the ordinary sequential NIP/cycle/timebase retirement effects.

The decoder and production `VirtualMachine::step()` tests cover ten source patterns at all counts 0–127; explicit distances 0, 1, 31, 32, 63, 64, 65, and 127; RB upper-bit independence; all 1,024 RS/RA combinations; RB aliases; R0 and R31; Rc behavior; XER SO set and clear; and preservation of GPRs, CR, XER, MSR, LR, CTR, SPRG1, RAM, interrupt state, LPID, and VM isolation.

Representative expected results include:

| Source | Count | Result |
|---|---:|---|
| `0x8000000000000000` | 1 | `0x4000000000000000` |
| `0x8000000000000000` | 63 | `0x1` |
| `0xFFFFFFFFFFFFFFFF` | 1 | `0x7FFFFFFFFFFFFFFF` |
| `0xFFFFFFFFFFFFFFFF` | 32 | `0x00000000FFFFFFFF` |
| `0xFFFFFFFFFFFFFFFF` | 63 | `0x1` |
| `0xFFFFFFFFFFFFFFFF` | 64 or 127 | `0` |
| `0x2373697A00000000` | 32 | `0x000000002373697A` |

Standalone mode coverage exercised SF=0/1 and supervisor/problem state. As in the accepted SLD POWER9 profile, the instruction is available in both SF modes and privilege states; no supervisor-only restriction or exception was introduced. Authentic guest replay executed in LPID 1. Standalone instruction VMs use their default LPID 0. State version 6 and serialized state size are unchanged.

## SLD audit and decoder regression

The existing SLD implementation was audited and left semantically unchanged. It snapshots RS and RB before the RA write, uses the same seven-bit count contract with a guarded shift, updates CR0 only for Rc=1, and advances NIP normally. Phase 38 reran the Phase 37 guest SLD matrices as regressions. The Phase 37 Linux replay was updated to cross the now-supported SRD and verify its live result.

Phase 38's 50,357 total checks include 18,495 reused SLD regression checks. The distinct Phase 38 count is therefore **31,862 checks**, with 58 Linux replay checks included and zero failures. The refreshed Phase 37 executable reports 18,577 checks (18,575 focused; 60 Linux replay checks included; zero failures), six more assertions than its accepted Phase 37 report because it now verifies the actual SRD retirement after SLD.

## Authenticated guest identity and exact SRD retirement

The unchanged inputs passed the established verifier:

- Kernel SHA-256: `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd`.
- Configuration SHA-256: `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa`.
- FDT SHA-256: `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37`.
- CPU: POWER9 DD2.0, PVR `0x004E1200`.
- Linux partition: LPID 1; original ELF placement, entry state, radix/real translation, FDT, RTAS-disabled probe, and PACA/SPRG1 state were retained.

The authenticated prefix from kernel entry reproduced its accepted digest `0x25125e2f8e651098` over 2,641,070 prefix retirements. Immediately before SLD at `0xC000000000170884`, R9 was `0x6C6C73002373697A` and R6 was `0x20`; SLD produced R9=`0x2373697A00000000` at NIP `0xC000000000170888`.

The actual pre-SRD snapshot then captured:

- NIP: `0xC000000000170888`.
- Fetched word: `0x7D293436`, decode `srd r9,r9,r6`.
- R9: `0x2373697A00000000`.
- R6: `0x0000000000000020`; low-seven-bit distance 32.
- Independent unsigned expected R9: `0x000000002373697A`.
- Production post-SRD R9: `0x000000002373697A`.
- Post-SRD NIP: `0xC00000000017088C`.
- CR, XER, MSR, and SPRG1 unchanged; RAM fingerprint remained `0x94517cce73b9deb2`.
- Cycles and timebase each advanced from 2,654,912 to 2,654,913.

No register was manually seeded to create the Linux state. The check captured R6 again at SRD. Both fresh production VM runs agreed on every listed state item, kernel/FDT identity, prefix and continuation digests, final CPU/RAM state, retirement count, and frontier. The continuation digest was `0x0E8C3428D8FE0D4F`; final total was 2,654,917 authentic instructions.

The guest continued through the second SRD at `0xC00000000017088C`, compared the resulting values with `cmpld` at `0xC000000000170890`, then stopped at:

```text
NIP:          0xC00000000017089C
word:         0x20C60008
independent:  subfic r6,r6,8
classification: unsupported instruction
```

This is the next reproducible unsupported-opcode frontier, not a budget endpoint, guest exception, or boot-completion claim. The stop is later than the old SRD frontier. No subsequent instruction was implemented as part of Phase 38.

## SLD/SRD dataflow and source attribution

The SLD and SRD are consecutive, use the same live R6 value `0x20`, and transform the same R9 register from `0x6C6C73002373697A` to `0x2373697A00000000`, then to `0x000000002373697A`. The surrounding instructions had loaded the FDT string-table bytes into R9 and a kernel literal into R10. SRD shifts R10 right by 32 as well; the subsequent compare sees `0x000000002373697A` in both registers.

The authenticated FDT bytes at file/guest offset `0x300` begin `6c 6c 73 00 23 73 69 7a` (`lls\0#siz`); `#size-cells` starts at offset `0x304`. The unchanged Linux ELF maps the literal `#size-cells\0` at virtual address `0xC0000000017A9468` (file offset `0x17B9468`). This establishes a four-byte comparison of the FDT property-name chunk `#siz` against the Linux literal `#siz`, followed by another compare and loop test.

Linux v7.2 `early_init_dt_scan_root()` looks up the root `#size-cells` property. libfdt's `fdt_getprop()` delegates to property lookup, which scans properties and compares names through `fdt_string_eq_()`. The observed dataflow is strongly consistent with that property-name comparison in early device-tree parsing. The ELF inspector reports no enclosing symbol for the instruction address, so the report does not claim an exact compiled function binding or source line for this instruction sequence. The value being compared here is a property-name string chunk, not the property value or an integer size.

Sources: [Linux v7.2 `early_init_dt_scan_root`](https://github.com/torvalds/linux/blob/v7.2/drivers/of/fdt.c#L936-L955), [Linux v7.2 libfdt property-name comparison](https://github.com/torvalds/linux/blob/v7.2/scripts/dtc/libfdt/fdt_ro.c#L98-L105), and [Linux v7.2 libfdt property lookup](https://github.com/torvalds/linux/blob/v7.2/scripts/dtc/libfdt/fdt_ro.c#L376-L402). The ISA reference is Power ISA 3.1C; pinned source comparison used QEMU commit `1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6` and Linux v7.2 opcode definitions. No QEMU runtime equivalence is claimed.

## Programs A–N

- **A — Exact frontier decode:** passed; all opcode and register fields match `0x7D293436`.
- **B — General X-form decode:** passed; Rc and neighboring instruction isolation verified.
- **C — Seven-bit shifts:** passed all distances 0–127 over ten patterns through `VirtualMachine::step()`.
- **D — Boundaries:** passed counts 0, 1, 31, 32, 63, 64, 65, 127.
- **E — High RB bits:** passed high-bit independence, including RB bit 63 and `0xFFFFFFFFFFFFFFC0`.
- **F — Logical zero fill:** passed; no sign extension and correct word-half crossing.
- **G — Register combinations:** passed all 1,024 RS/RA pairs plus RA/RB, RS/RB, all-equal, R0, and R31 aliases.
- **H — Record form:** passed Rc=0 preservation and Rc=1 CR0 LT/GT/EQ/SO behavior; CR1–CR7 preserved.
- **I — Architectural state:** passed XER, MSR, LR, CTR, SPRG1, RAM, interrupt, retirement, and checkpoint-version assertions.
- **J — Modes and isolation:** passed SF=0/1 and supervisor/problem tests; authentic LPID 1 and separate VM state verified.
- **K — Decoder regressions:** passed SLD and existing rotate/shift decoder checks.
- **L — Production VM stepping:** all guest-facing SRD vectors execute through `VirtualMachine::step()`.
- **M — Authentic Linux replay:** passed; real SLD and SRD retired with the captured live operands and result.
- **N — Continuation and regressions:** passed bounded continuation to the next unsupported opcode and two-run deterministic comparison.

## Regression results

- Accepted PPC64 Phases 1–35 baseline: 1,289,608 Debug checks.
- Accepted refreshed Phase 36 baseline: 1,318 checks.
- Accepted Phase 37 baseline: 18,571 checks; refreshed Phase 37 with SRD retirement assertions: 18,577 checks.
- Phase 38 total: 50,357 checks; 18,495 checks are the explicitly reused Phase 37 SLD regression subset; 31,862 are distinct Phase 38 checks.
- Refreshed distinct total through Phase 38: **1,341,365 checks** (= prior 1,309,497 + six new Phase 37 SRD assertions + 31,862 distinct Phase 38 checks). Debug and Release are not double-counted.

Full Debug CTest registered 53 tests: 47 passed, six historical executables were Not Run, and zero executed tests failed. The actual CTest exit code was 8 because those six executables are absent: `test_syscall_dispatcher`, `test_debugger`, `test_profiler`, `test_fuzzer`, `test_vm_isolation`, and `test_elf_validation`. All available PPC64 tests from Phase 1 through Phase 38 passed, including Phases 21, 24, 25, 29–37, the Phase 31 Linux replay, and the Phase 38 replay.

Release CTest selected VM checkpoint plus PPC64 Phases 35–38: **5/5 passed**, exit code 0. Debug VM checkpoint passed in the full suite. Debug IA-64 emulator/SAL and new-instruction tests passed in CTest; standalone IA-64 ACPI, interrupt-controller, and processor-interrupt-block executables each exited 0. No shared IA-64 `.phase` was modified. Existing Windows socket build limitations and absent historical tests were not repaired as part of this instruction milestone.

## Artifacts, limitations, and PPC64-39

Phase 37 evidence and its manifest remain intact. The Phase 37 manifest SHA-256 remains `a1f3c0ec0a70d2bff3789ecc89e94e855da53e2a0936d9b5b6984b5cddf77a02`. Phase 38 evidence is under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase38\`, with separate `debug-run-final`, `release-run-final`, and `phase37-regression-debug` runs. It contains the verifier output and input hashes, independent vectors and A–N summary, pre-SLD and authentic SRD snapshots, Linux dataflow/source inspection, bounded continuation, two-run comparison, Debug and Release CTest logs, IA-64 logs, per-run SHA-256 manifests, and a Phase 38 SHA-256 manifest. The Phase 38 manifest has 53 entries and SHA-256 `85d98c9d29ec95477bf26b20b8d0a90dab262485853ca54a875716712a5265b7`.

Remaining limit: Linux stops at `subfic r6,r6,8` (`0x20C60008`) because that instruction is unsupported in this bounded profile. No Linux boot-completion claim is made. QEMU runtime remains `QEMU_REFERENCE_PENDING`.

**PPC64-39 recommendation:** implement only the newly observed `subfic` (opcode 8) after independently verifying the exact instruction and its actual live operands, then replay the unchanged guest from kernel entry, preserve the current artifact and regression gates, and stop at the next unsupported instruction or justified bounded endpoint. Do not infer the SUBFIC result from the earlier SRD path or modify the guest artifacts.
