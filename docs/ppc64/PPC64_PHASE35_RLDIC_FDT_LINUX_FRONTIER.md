# PPC64 Phase 35 — RLDIC and Linux FDT Frontier

**Result: Outcome A — PPC64-35 complete.** General MD-form RLDIC decoding and execution passed the independent production-VM suite. The authenticated Linux RLDIC retired, the kernel continued through the libfdt tag parser, and two fresh VM lifecycles reached the same next unsupported instruction. This does not claim a complete Linux boot or QEMU runtime parity.

## Repository preflight and closeout

- Workspace: `D:\dev\guideXOS_Hypervisor_PPC64`; branch: `PPC64`.
- Starting HEAD: `fa14675e5f71d2e6cfe4b89355db464d7d4d3a7f`, exactly the requested expected start. Before edits the branch tracked `origin/PPC64` with 0 ahead / 0 behind, and the worktree was clean.
- The ignored `build/` directory was preserved. No reset, rebase, merge, stash, clean, cherry-pick, push, branch switch, or sibling-worktree change was made.
- The architecture marker started at PPC64-34 / PPC64-35 and advanced to PPC64-35 / PPC64-36 after qualification.
- Shared IA-64 `.phase` remains `last_completed=IA64-POST-MADT-6`, `next_expected=IA64-POST-MADT-7`.
- Phase 34 evidence was left intact. The local checkout and its authenticated guest inputs were authoritative.

## References and encoding

The normative reference reviewed was [OpenPOWER ISA 3.1C](https://openpowerfoundation.org/specifications/isa/). Source cross-checks used [the pinned QEMU fixed-point translator](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate.c), [its MD-form decode definitions](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/insn32.decode), [Linux v7.2 opcode definitions](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/include/asm/ppc-opcode.h), [Linux v7.2 libfdt tag parsing](https://github.com/torvalds/linux/blob/v7.2/scripts/dtc/libfdt/fdt.c), and [the dcbt instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-dcbt-data-cache-block-touch-instruction). The inspected QEMU commit is `1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6`. QEMU was used as a source-level reference only; its runtime was not launched.

The authentic word is `0x7BA91788`, at effective NIP `0xC000000001388490`. The independent decode is `rldic r9,r29,2,30`: primary opcode 30, MD operation 2, RS=29, RA=9, SH low five bits 2, SH high bit 0, MB low five bits 30, MB high bit 0, Rc=0. The decoder joins SH as `SHlow5 | (bit1 << 5)` and MB as `MBlow5 | (bit5 << 5)`. Opcode 30 operation 2 is dispatched distinctly from RLDICL, RLDICR, and RLDIMI; other MD operations are not routed through RLDIC.

For all decoded operands, ME is `63 - SH`, rotation is full-width unsigned `ROTL64(RS, SH)`, and the result is `rotated & MASK64(MB, ME)`. Power bit numbering is MSB-first. When MB exceeds ME, the helper selects both ends of the word and correctly wraps around.

| SH | MB | ME | Expected mask |
|---:|---:|---:|---|
| 0 | 0 | 63 | `0xFFFFFFFFFFFFFFFF` |
| 0 | 63 | 63 | `0x0000000000000001` |
| 2 | 30 | 61 | `0x00000003FFFFFFFC` |
| 32 | 0 | 31 | `0xFFFFFFFF00000000` |
| 32 | 63 | 31 | `0xFFFFFFFF00000001` |
| 63 | 0 | 0 | `0x8000000000000000` |
| 63 | 63 | 0 | `0x8000000000000001` |

Unlike RLDIMI, RLDIC writes only the rotated masked value and clears every unselected destination bit. The implementation reads the original RS before writing RA, so RA=RS is safe. Rc=1 updates CR0 using the final masked result and XER[SO]; CR1–CR7 remain intact. XER, MSR, SPRG1, LR, CTR, unrelated GPRs, and guest memory are preserved by the instruction. No checkpoint state or format changed.

The mode matrix exercised supervisor/problem state with SF clear/set (four combinations) through production VM stepping. Rc and non-Rc paths, GPR0/GPR31, aliases, record results, and CR field preservation passed.

## Focused qualification

The oracle independently implements Power's MSB-numbered mask, safe 64-bit rotation, and AND operation; it does not call the production RLDIC handler. Debug and Release each recorded **797,261 checks**: 797,175 focused checks and 84 authentic Linux replay checks, zero failures.

- All 4,096 SH/MB pairs were decoded and stepped through the production VM for eight source patterns each: 32,768 steps. Patterns included zero, all ones, the high bit, the low bit, mixed bits, both alternating patterns, and deterministic mixed bits.
- All 1,024 RS/RA pairs were exercised with representative immediates, including source/destination aliases and GPR0/GPR31. Dedicated alias cases verified RLDIC clears the seeded destination outside the selected mask.
- Rotation boundaries 0, 1, 2, 15, 16, 31, 32, 33, 47, 48, 62, and 63; both split immediate high bits; ordinary masks; wrapping masks; Rc=0/1; and all four supervisor/problem × SF mode combinations passed.
- Programs A–N all pass:
  A exact Linux MD-form decode; B MD operation isolation; C split six-bit SH/MB; D rotation boundaries; E non-wrapping masks and destination clearing; F wrapping masks; G 4,096-pair matrix × 8 patterns; H 1,024 register pairs and aliasing; I CR0 record values and CR1–CR7 preservation; J XER/MSR/SPRG1/LR/CTR plus privilege/SF modes; K RLDICL/RLDICR/RLDIMI/RLWINM regressions; L production `VirtualMachine::step()`; M authentic Linux retirement; N bounded continuation, FDT integrity, and repeatability.
- Decoder and disassembly evidence, immediate matrix results, register dataflow, live state, FDT checks, continuation trace, and repeat comparison are stored under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase35\debug-run-final`.

## Authenticated Linux execution and FDT dataflow

The artifact verifier confirmed the unmodified Linux ELF64 big-endian PPC64 ET_EXEC identity, entry `0xC000000000000000`, and accepted load segment. The same POWER9 DD2.0 PVR `0x004E1200`, LPID 1, virtual-time progression, radix partition configuration, original boot entry state, and FDT were used for both runs. Both VM lifecycles started at the kernel entry and reproduced the relocation/PACA/SPRG1 and Phase 34 EXTSW prefix; no checkpoint skipped guest execution.

| Artifact | SHA-256 |
|---|---|
| Kernel `vmlinuz-7.2.14+deb14-powerpc64` | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Kernel configuration | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| FDT | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |

The authenticated package record identifies binary package `linux-binary-7.2.14+deb14-powerpc64`, version `7.2.14-1`, source package `linux`. The candidate contains no unpacked Debian source tree, so no exact Debian patch-level source attribution is claimed; the matching package identity, ELF symbol/dataflow, and upstream Linux v7.2 source are kept distinct. The package record and provenance note are preserved in the regression artifacts.

ELF inspection attributes the instruction to `fdt_next_tag+0x70`. Dynamic dataflow established that R29 was loaded from the FDT tag at effective/guest-real/backing address `0x7ff0058`; the big-endian tag was `FDT_BEGIN_NODE` (1). Before RLDIC, R9 held `0xC000000001386000`. The independent expected result is `ROTL64(1,2) & 0x00000003FFFFFFFC = 4`; production R9 afterward was exactly 4. The following `lwax` accessed effective address `0xC0000000013884AC`, which translated to guest-real/backing `0x13884AC`; its table word was signed offset `0x38`. The computed target `0xC0000000013884E0` was reached with `mtspr 9,r9` and `bcctr`, selecting the FDT_BEGIN_NODE handler. This establishes the parser's observed dispatch, rather than inferring acceptance from retirement alone.

| Live state | Value |
|---|---|
| NIP / raw word / disassembly | `0xC000000001388490` / `0x7BA91788` / `rldic r9,r29,2,30` |
| R29 / R9 before | `0x0000000000000001` / `0xC000000001386000` |
| Independent mask / expected R9 | `0x00000003FFFFFFFC` / `0x0000000000000004` |
| Actual R9 after / next NIP | `0x0000000000000004` / `0xC000000001388494` |
| CR before / after | `0x84000000` / `0x84000000` |
| XER before / after | `0x20040000` / `0x20040000` |
| MSR / SPRG1 / LPID | `0x8000000000000002` / `0xC000000001F42380` / 1 |
| VM cycles and timebase before / after | 2,651,596 / 2,651,597 |
| Guest RAM fingerprint at RLDIC before / after | `0x69c13e6d89c67649` / `0x69c13e6d89c67649` |

The authenticated prefix to RLDIC was reproduced with 2,651,596 prior retirements; RLDIC retired once. Linux then retired 609 further instructions, for **2,652,206 total retirements**, before the next unsupported/invalid instruction. The last successful NIP was `0xC0000000001703BC`; the next frontier is `0xC000000000170424`, raw word `0x7C001A2C`. Independent X-form decoding gives primary opcode 31, TH=0, RA=0, RB=3, XO=278, Rc=0: `dcbt r0,r3,0` (Data Cache Block Touch). The current plugin reports it unsupported. Its ELF symbol attribution is unknown; caller attribution is `early_init_dt_scan_root`. The bounded stop is not presented as a Linux failure and the next instruction was not implemented.

The FDT is a 921-byte authenticated backing region at effective, guest-real, and backing address `0x07FF0000`. Header parsing confirmed magic `0xD00DFEED`, total size 921, structure block offset 88/size 640, strings offset 728/size 193, version 17, 35 tags, aligned/bounded traversal, and FDT_END. The RLDIC step preserved guest RAM and FDT bytes; the FDT FNV-64 fingerprint stayed `0x1F28C5F1B0B64BEB`. Subsequent parser execution reached the root scan helper without a demonstrated FDT divergence.

Two independent VM lifecycles agree on kernel/config/FDT hashes, R29 and R9 pre/post state, post-RLDIC NIP, 609 following retirements, trace digest, final CPU, final RAM, FDT backing, and next frontier. They both stopped at `0xC000000000170424` with word `0x7C001A2C`. This is a bounded libfdt execution result, not a complete boot.

## Regression qualification and artifacts

Debug CTest registered 50 tests: **44 passed, 0 executed failures, 6 Not Run**, exit code 8. The six absent executables are `test_syscall_dispatcher`, `test_debugger`, `test_profiler`, `test_fuzzer`, `test_vm_isolation`, and `test_elf_validation`; CTest lists them as Not Run, not passes. Every available PPC64 Phase 1–35 test, the VM checkpoint test, and IA-64 SAL passed. The standalone Debug IA-64 ACPI, interrupt-controller, and Processor Interrupt Block executables also passed.

The first rebuilt regression pass found stale decoder/frontier fixtures. Phase 21, 25, 26, 27, and 30 now require opcode-30 operation 2 to decode as RLDIC while preserving their assertion counts. Phase 31 and 32 Linux replay expectations now track the observed later frontier after the supported SPRG1, EXTSW, and RLDIC instructions. All seven refreshed Debug targets passed before the full CTest rerun. PPC64 Phases 1–34 retain the accepted **492,347 assertions**: 479,228 for Phases 1–33 and 13,119 for Phase 34. CPU-state version 6, the v1–v5 readers, and the VM checkpoint regression remain passing; RLDIC adds no checkpoint fields.

The all-target Debug build stopped at the unrelated `test_mov_imm` link target because its shared WebUIServer object had unresolved Windows socket imports (`__imp_socket`, `__imp_bind`, `__imp_send`, and related symbols). The task-relevant PPC64 Phase 1–35, Phase 31 Linux replay, VM checkpoint, and four IA-64 targets were built directly and linked successfully. No unrelated CMake/link repair was made.

The final Phase 35 suite was rerun after adding the independent frontier decode: Debug CTest passed 1/1 in 43.96 seconds and Release CTest passed 1/1 as part of a **7/7** focused Release group. The Release group comprised Phase 21 RLDICR (2,443 checks), Phase 24 RLWINM (809), Phase 25 RLDICL (813), Phase 30 RLDIMI (205,022), Phase 35 (797,261), VM checkpoint, and IA-64 SAL. Release Phase 35 recorded 797,175 focused checks plus 84 Linux replay checks, zero failures. The Release standalone IA-64 ACPI, interrupt-controller, and Processor Interrupt Block executables all passed. The complete Phase 1–35 and shared CTest inventory was run in Debug; Release verification targeted Phase 35 and the affected rotate/merge/checkpoint/IA-64 suites.

The regression output also preserves updated counts for Phases 1–34: **492,347 PPC64 assertions**, comprising 479,228 for Phases 1–33 and 13,119 for Phase 34. Phase 35 adds 797,261, bringing the total qualified checks through Phase 35 to 1,289,608. No checkpoint data or version changed.

Artifacts are outside Git:

- Debug run: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase35\debug-run-final`; manifest SHA-256: `1eb9c3c6f71bba5f44d06117a82a81ec67c65802e29d695f1a0d6c88992b0dd4`.
- Release run: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase35\release-run-final`; manifest SHA-256: `4a3bc9300fe9878d9067c0590605130e7612db58585518dbf6ff6d911e1c84f0`.
- Debug full CTest, refreshed target tests, focused Phase 35 rerun, Release focused CTest, Debian package provenance, and standalone IA-64 logs are under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase35\regressions`; `regression-sha256-manifest.txt` hashes to `75726c2ad82f9013d7020f395809d888e51637a78afd52219086f637a292c5a1`.

No large generated artifact is committed.

`git diff --check` passed. QEMU runtime remains `QEMU_REFERENCE_PENDING`; QEMU/QAC was not run.

## PPC64-36 recommendation

Start from the reproducible unsupported `dcbt r0,r3,0` at `0xC000000000170424` (`0x7C001A2C`, TH=0, RA=0, RB=3) in the `early_init_dt_scan_root` call path. Qualify its cache-hint behavior, mode/exception requirements, and safe no-op or cache model before considering implementation. Preserve the Phase 35 two-run trace, authenticated Linux/FDT hashes, 2,652,206-retirement endpoint, and all accepted Phase 1–35 regressions. Do not infer a full boot milestone from this bounded continuation.
