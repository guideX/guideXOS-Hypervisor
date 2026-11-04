# PPC64 Phase 29 — Linux Relocation Walk and New Frontier

**Outcome: A1 — the data walk exited and Linux reached a new unsupported instruction.** The authentic Linux image processed its complete dynamic relocation table, returned from `relocate`, and continued through `initialise_paca`. The first unretired instruction is an observed `rldimi` at `initialise_paca+0x60`. No CPU instruction implementation was changed in this phase.

## Repository and run identity

- Checkout: `D:\dev\guideXOS_Hypervisor_PPC64`.
- Branch: `PPC64`, tracking `origin/PPC64`; starting HEAD `2e63c4bcde0269caf3ce4363a09513b110c50276`, 0 ahead / 0 behind.
- Starting tracked and untracked worktree was clean. The ignored `build/` was preserved. Starting `.phase-ppc64` named PPC64-28 as complete and PPC64-29 as next. The shared IA-64 `.phase` was not touched.
- Kernel and FDT verifier passed. The existing Phase 28 trace was read in place and not overwritten.
- Production guest instruction execution used `VirtualMachine::step()`. There was no QEMU runtime, guest memory patch, forced loop exit, kernel rebuild, or checkpoint.

| Input | Identity |
|---|---|
| Kernel | `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64` |
| Kernel SHA-256 | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Configuration SHA-256 | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| FDT SHA-256 | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |
| CPU | POWER9 DD2.0, PVR `0x004E1200` |
| Entry and translation | NIP `0`; direct FDT; RTAS disabled; LPID 1 with the configured POWER9 partition radix table |

The accepted Linux load extent remains `[0,0x29C2158)`. The run used a 128 MiB guest, `SF=1`, `HV=0`, `IR=0`, and `DR=0`, with the established guest-real mask and private identity backing range `[0,0x8000000)`.

## Phase 28 replay

The first 16,384 production step records compare byte-for-byte with the preserved Phase 28 trace, whose SHA-256 is `fbdea1e752efae8f3937f507adb4696dde229ecc8cd53beaa9e3a046275ed365`. The complete saved architectural state at that point also matches: all 32 GPRs, NIP, LR, CTR, CR, XER, MSR, active LPID, and virtual timebase. The first 246 instructions and the DIVD operation remain identical.

At NIP `0xC0000000000538A4`, the authentic word `0x7D0863D2` retired as `divd r8,r8,r12`: R8 changed from `0x54E3D0` to `0x3897E`, with R12=`0x18`. The quotient is the runtime relocation-entry count.

## ELF and Linux attribution

The verified image is ELF64, big-endian, EM_PPC64, ET_EXEC. Its ELF contains `.symtab` and `.strtab`; there was no separate `System.map` dependency. GNU `readelf` showed the single load segment at file offset `0x10000`, linked address `0xC000000000000000`, physical address zero, file size `0x28865E4`, and memory size `0x29C2158`. The section-to-segment flags are RWE. `.text` has linked address `0xC000000000008000`, file offset `0x18000`, and size `0x1405F54`.

For file-backed addresses in this segment, file offset is `0x10000 + (linked address - 0xC000000000000000)`. Thus the repeated code at VA `0xC0000000000538AC` is at file offset `0x638AC`, not at file offset `0x538AC`.

The ELF `.rela.dyn` section starts at VA `0xC000000002029C60`, file offset `0x2039C60`; it is `0x54E3D0` bytes with 24-byte entries, or 231,806 records. Runtime R9 started at that section address and ended at `0xC000000002578030`, exactly start plus `231806 * 24`, immediately after the table. The authenticated artifact manifest independently records 231,806 `.rela.dyn` entries and the Debian source package as Linux `7.2.14-1`.

The binary’s full symbol table identifies `relocate` at `0xC000000000053808`. The live instructions at `0xC0000000000538AC` match its dynamic relocation loop. Upstream Linux v7.2 `arch/powerpc/kernel/reloc_64.S` provides the matching source algorithm: derive the record count using the RELA entry size, seed CTR, process `R_PPC64_RELATIVE`, advance by 24 bytes, and use `bdnz` for the loop. Linux startup calls this relocation routine. Debian’s source package is versioned separately; the authenticated Debian binary and its runtime behavior are the execution authority, and no claim of byte-for-byte identity with unpatched upstream is made.

- [Linux v7.2 PowerPC relocation routine](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/reloc_64.S)
- [Linux v7.2 PowerPC early entry](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/head_64.S)
- [Debian Linux 7.2.14-1 source package](https://sources.debian.org/src/linux/7.2.14-1)
- [OpenPOWER ISA specifications](https://openpowerfoundation.org/specifications/isa/)

## Observed code and dataflow

The repeating relative-relocation path has ten instructions per record:

| NIP | Word | Production decode | Observed role |
|---|---:|---|---|
| `0x...538AC` | `E8090008` | `ld r0,8(r9)` | Load `r_info` |
| `0x...538B0` | `2C200016` | `cmpdi r0,22` | Compare relocation type against 22 |
| `0x...538B4` | `40820010` | `bc 4,2,0x...538C4` | Dispatch other relocation types; not taken for these records |
| `0x...538B8` | `E8C90000` | `ld r6,0(r9)` | Load `r_offset` |
| `0x...538BC` | `E8090010` | `ld r0,16(r9)` | Load `r_addend` |
| `0x...538C0` | `4800002C` | `b 0x...538EC` | Skip the alternate-type path |
| `0x...538EC` | `7C001A14` | `add r0,r0,r3` | Add relocation delta |
| `0x...538F0` | `7C07312A` | `stdx r0,r7,r6` | Store relocated value at base plus offset |
| `0x...538F4` | `7D296214` | `add r9,r9,r12` | Advance RELA pointer by 24 bytes |
| `0x...538F8` | `4200FFB4` | `bc 16,0,0x...538AC` | Decrement CTR and branch while nonzero |

The first entry to `0x...538AC` was attempt 248; the first back-edge was attempt 257. These NIPs each executed 231,806 times. The type dispatch branch was not taken 231,806 times; the unconditional branch to the add/store block was taken 231,806 times. The CTR back-edge was taken 231,805 times and fell through once.

At loop entry, R9=`0xC000000002029C60`, R12=`24`, R3=`0`, R7=`0`, CTR=`0x3897E`, and LR=`0xC00000000000D098`. The runtime type loaded into R0 was 22 for every record, and the compare set the expected CR0 value each time. Each R9 step was exactly +24; R3, R7, and R12 stayed invariant. CTR decreased to zero after exactly the runtime count, after which the loop fell through to `blr` at `0xC0000000000538FC` and returned to LR. All progress and termination evidence comes from the guest instructions and register state, not a host-simulated loop.

Samples from iterations 1, 2, 3, 16, 256, and 1,024 are in `loop-register-dataflow.txt`. The final pointer spans the complete `.rela.dyn` section. The dynamic input therefore establishes the bound; the test does not infer termination from NIP repetition or force a fixed iteration limit.

## Memory access and integrity results

Every decoded loop load/store was independently translated using its effective address, actual access type, active LPID, partition context, and required permission. The table below retains high effective addresses, then reports the corresponding guest-real address and backing address. With the established real-mode mask, the low 60 bits become GRA; this VM’s partition radix maps that GRA to the same backing address.

| Operation | Count | First EA | Last EA | GRA range | Distinct 4 KiB pages |
|---|---:|---|---|---|---:|
| 64-bit reads | 695,418 | `0xC000000002029C68` | `0xC000000002578028` | `0x2029C60`–`0x2578028` | 1,360 |
| 64-bit writes | 231,806 | `0xC00000000002BE98` | `0xC0000000028683E8` | `0x2BE98`–`0x28683E8` | 1,047 |

All accesses were aligned, permitted, and within the 128 MiB RAM backing. All write targets fell inside the loaded kernel image. Each store target matched the loaded `r_offset` plus R7, and each stored R0 value matched the instruction’s source. Every target was unique; all 231,806 writes contained the same 64-bit value before and after. The relocation delta and target base were both zero, so this runtime relocation pass changed no memory contents.

Two relocation targets fall inside the ELF `.text` section. The ELF load segment is RWE for the early relocation path; these are expected stores from the authenticated `relocate` routine. Both retained their previous value, and no executable-section content changed. The test separately verifies no changed store value anywhere, no FDT or RTAS writes, and unchanged FDT backing. The FDT hash and FDT bytes match the accepted input.

## Adaptive execution and final frontier

Production runs observed the following stage snapshots. Stage limits are maximum retired counts. Execution stopped earlier when the guest reached a new unsupported instruction.

| Retired stage | NIP after stage | CTR | Loop entries | Writes observed |
|---:|---|---:|---:|---:|
| 16,384 | `0xC0000000000538EC` | `0x38331` | 1,614 | 1,613 |
| 65,536 | `0xC0000000000538F4` | `0x36FFE` | 6,529 | 6,529 |
| 262,144 | `0xC0000000000538EC` | `0x32331` | 26,190 | 26,189 |
| 1,048,576 | `0xC0000000000538F4` | `0x1EFFE` | 104,833 | 104,833 |
| 4,194,304 maximum | Not reached | — | — | — |

The loop exited and returned after 2,318,309 retired guest steps (the `blr` attempt follows the final recorded back-edge). Linux continued executing. The final replay retired 2,641,036 instructions and attempted 2,641,037. Its last retired NIP was `0xC000000001EAB1CC`, word `0xB0830038`. At NIP `0xC000000001EAB1D0`, the production VM fetched `0x796A000E` but rejected it as unsupported primary opcode 30, so it did not retire. The independent field decode is `rldimi r10,r11,32,0`. The linked address is inside the ELF `initialise_paca` symbol at `0xC000000001EAB170`, offset `+0x60`; file offset is `0x1EAC1D0` under the verified load map.

This is a new instruction frontier, not a budget endpoint, memory fault, or stable wait. It occurs after the relocation loop completed. The guest has not completed a Linux boot.

## Determinism, isolation, diagnostics

Two fresh production VM lifecycles with the same verified kernel, configuration, FDT, load map, PVR, entry registers, LPID context, virtual time setup, and budget agreed on:

- All first 16,384 trace records byte-for-byte.
- DIVD operands and result; loop entry, sampled states, and exit.
- Full retired architectural-state digest and final register view.
- Per-NIP and branch frequencies, memory counts/ranges, store side effects, stage snapshots, and memory digest.
- Final unsupported NIP, raw word, fetched/decoded/retired accounting, and stop reason.

During VM A’s extended run, VM B retained a distinct GPR14 sentinel, two-word memory fingerprint, active LPID 1, register view, and independent XICS snapshot with no pending interrupts. The harness also compared all VM A store target backing locations against VM B. No cross-VM mutation was observed.

Diagnostic growth is bounded: the exact Phase 28 prefix is retained, the endpoint keeps only 64 compact instruction records, register samples are periodic, and at most 96 loop memory samples are serialized. Aggregate frequency and range counters cover the full run. No RAM dump or million-line instruction trace was produced.

## Programs A–N

| Program | Result |
|---|---|
| A — Phase 28 reproduction | Pass; exact trace prefix and full saved state match. |
| B — Loop identification | Pass; ten-instruction path, branch blocks, frequencies, and back-edge identified from live NIPs. |
| C — Register dataflow | Pass; RELA pointer, 24-byte stride, relocation delta/base, and CTR state captured and checked. |
| D — Exit predicate | Pass; type-22 CR comparison, conditional dispatch, CTR decrement/`bdnz`, zero exit, and `blr` return observed. |
| E — Memory attribution | Pass; actual 64-bit EA/GRA/backing accesses translated with permissions. |
| F — Forward progress | Pass; pointer reached the exact end of `.rela.dyn` and CTR reached zero. |
| G — Instruction semantics | Pass; compare/branch, load/store, add, pointer, and counter transitions matched independent expectations. |
| H — Adaptive execution | Pass; Linux continued beyond Phase 28 and reached a new authenticated frontier under the 4,194,304 maximum. |
| I — Trace accounting | Pass; attempted, fetched, decoded, retired, lookahead, and stop state are separate. |
| J — Diagnostic bounds | Pass; samples and final trace window are bounded. |
| K — Memory integrity | Pass; all stores were expected relative relocations, values were unchanged, and protected data remained intact. |
| L — VM/LPID isolation | Pass; observer registers, memory fingerprint, LPID, interrupt snapshot, and touched memory remained stable. |
| M — Repeatability | Pass; two fresh runs match through the full new frontier. |
| N — Regressions and PPC64-30 handoff | Pass; see regression record and next-phase recommendation below. |

## Regressions and limits

The previously accepted PPC64 Phases 1–28 baseline of 17,505 assertions remains passing. The Phase 29 executable reports **248,502 total checks, 0 failures** (248,499 within the focused Linux-data-walk section). Adding the Phase 29 total gives 266,007 PPC64 phase checks across the accepted suite.

Full CTest contained 43 entries: **37 passed, zero executed failures, and six Not Run** because the shared executables are absent: `SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, and `ELFValidationTests`. CTest reports a nonzero status for those Not Run entries. All available PPC64 Phases 1–29, `VMCheckpointTests`, `IA64SalTests`, and both console tests passed. The standalone IA-64 ACPI (seven checks), interrupt controller, and Processor Interrupt Block tests also passed. The historical Phase 16 slow-reader timing test passed in CTest.

No checkpoint was needed. QEMU remains `QEMU_REFERENCE_PENDING`; no QEMU runtime was used and no runtime parity is claimed. There was no full Linux boot. The only execution blocker observed in this replay is `rldimi` at the new frontier.

## Artifacts and hashes

The preserved Phase 28 files remain under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase28\ctest-final\run-1793798021385\`. The accepted Phase 29 run and generated SHA-256 manifest are under:

`E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase29\run-1793832857319\`

That run contains the verifier output, kernel/FDT hashes, original trace comparison, loop samples and frequency report, translated memory report, ELF/source attribution, adaptive stages, final instruction window, frontier report, two-run comparison, A–N result, regression summary, and artifact manifest. The manifest lists each artifact’s SHA-256. Key values are summarized here; the complete per-file list is in `artifact-sha256-manifest.txt`.

| Artifact | SHA-256 |
|---|---|
| Kernel | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| FDT | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |
| Preserved Phase 28 trace | `fbdea1e752efae8f3937f507adb4696dde229ecc8cd53beaa9e3a046275ed365` |
| Loop register/dataflow report | `fd704dffb16eedbb5c424036672b35233e9564dd364be483b9379e34b8258ce6` |
| Memory access attribution | `fc6aedc12dbd53cd837f25f2704f8b23a5b5f1f467bd375f33ef1d8bc3f19db9` |
| ELF attribution | `ca7204b431c94ca788af03a861cc75f3d545e84ffb4d664170bd8b02e480707e` |
| Frontier report | `ef71f47de808f4245d991f5d7e3a0428b00efee39f03af63e321b52ac3731cc3` |
| Final instruction window | `9cb491c9ff41eaec1c81b5b1089d94ea395178a41f8b599c98378fee2d0ca971` |
| Two-run comparison | `b4bd21eb2b0a4111c18e44899adad0e5642b4a69f1ad2da2e32ab649c45ae2ff` |
| Regression summary | `dc1b240734b5b50a81d257a433fd265dfffa43108ce2172b73ce7ee772db8bee` |
| Focused test results | `fc68a8926380d84a17094573279d29b4c9043e318f76e9aa4ac8108b7b79fb9c` |
| Phase 29 artifact manifest | `afb0f35b7d3f3edbcfcc6249ea0c0a12b212f582de792cb52967440749a33e4a` |

## PPC64-30 recommendation and completion

PPC64-30 should implement and qualify only the observed `rldimi` form family needed by the `initialise_paca` path, beginning with `0x796A000E` (`rldimi r10,r11,32,0`). Add independent Power ISA-derived decode and mask/insert vectors, then test execution through `VirtualMachine::step()` for the observed fields, boundary shifts/masks, register aliasing, CR/XER/MSR preservation, and unsupported neighboring forms. Resume the same authenticated kernel only after those tests pass, with a fresh bounded budget and the same memory/LPID and replay checks. Do not broaden into unobserved opcode families or claim full Linux boot.

On acceptance, `.phase-ppc64` is advanced to `last_completed=PPC64-29`, `next_expected=PPC64-30`. The shared IA-64 marker remains unchanged. The local completion commit leaves `PPC64` 1 ahead / 0 behind `origin/PPC64`; its SHA is recorded in the final completion response. No push or upstream operation was performed.
