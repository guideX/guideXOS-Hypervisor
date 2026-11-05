# PPC64 Phase 31 — TDI and authentic Linux boot-PACA continuation

**Outcome A1 — PPC64-31 accepted.** Generic PPC64 TDI semantics and Program-trap delivery are qualified through production VM steps. The authenticated Linux instruction did not trap because its real operand was zero. Linux retired TDI, returned from `fixup_boot_paca`, and reached a new unsupported instruction in `early_setup`. This is not a complete Linux boot.

## Repository and Phase 30 baseline

- Checkout: `D:\dev\guideXOS_Hypervisor_PPC64`, branch `PPC64` tracking `origin/PPC64`.
- Starting HEAD: `4fbe9eaef5a86f22164fbe80d2956e77400a4f23` (`PPC64-30 capture Linux frontier`); starting divergence was 0 ahead / 0 behind and the tracked/untracked worktree was clean. The ignored `build/` directory was retained.
- Starting `.phase-ppc64`: `last_completed=PPC64-30`, `next_expected=PPC64-31`. The shared IA-64 `.phase` was left unchanged.
- Phase 30's accepted report is [PPC64_PHASE30_RLDIMI_LINUX_CONTINUATION.md](PPC64_PHASE30_RLDIMI_LINUX_CONTINUATION.md). Its fresh replay is `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase30\run-1793920116028`; Phase 30 completed 205,023 checks with zero failures. The SHA-256 of its artifact manifest is `2f46431e709c60bea0a2d3fac6a6d4606c5ebb5e6e74e3247b6f72aa562415b6`.
- Phase 30 inputs: authenticated Debian Linux ELF SHA-256 `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd`; config `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa`; FDT `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37`.

## ISA semantics and focused qualification

The authentic word is `0x0B090000` at guest NIP `0xC000000001EAA2C8`. Independent field decoding gives primary opcode 2, TO=24, RA=9, SI=0, or `tdi 24,r9,0` (`tdnei r9,0`). The OpenPOWER ISA specification page identifies version 3.1C and its three books; Book I defines TDI and Book III defines exception handling: [OpenPOWER ISA specifications](https://openpowerfoundation.org/specifications/isa/).

The five TO bits are OR-combined:

| TO bit | Predicate |
|---:|---|
| 16 | Signed 64-bit RA less than sign-extended SI |
| 8 | Signed 64-bit RA greater than sign-extended SI |
| 4 | RA equal to sign-extended SI |
| 2 | Unsigned 64-bit RA less than sign-extended SI |
| 1 | Unsigned 64-bit RA greater than sign-extended SI |

The implementation compares signed values by mapping two's-complement ordering into unsigned ordering, so it never converts an out-of-range `uint64_t` to a signed host value. Unsigned predicates use the full 64-bit unsigned values. SI is sign-extended from 16 bits (`0x8000` becomes `0xFFFFFFFFFFFF8000`; `0xFFFF` becomes `0xFFFFFFFFFFFFFFFF`). TDI reads the actual GPR indexed by RA, including GPR0.

The focused suite passed **6,507 checks**, including 254 production VM-step vectors covering TO=0–31, every predicate bit and combinations, signed and unsigned edges, SI endpoints, RA=0/9/31, authentic no-trap behavior, true-trap entry, handler return, and neighboring TWI/TD/TW forms. The independent oracle does not reuse the production comparison helper.

For a satisfied predicate, TDI records `PROGRAM_TRAP` and enters the Program vector at `0x700` even when legacy host-only synchronous delivery is disabled. The tested entry saves the faulting CIA in SRR0, saves pre-entry MSR bits plus `SRR1_PROGTRAP` in SRR1, sets MSR[SF], and clears EE/PR/IR/DR/RI. The focused suite exercised 187 taken-TDI cases. A guest handler using `mtspr SRR0` and `rfid` returned to CIA+4; a missing vector fetch then produced the normal ISI path. POWER9 documentation lists the Program vector as `0x700` and describes the exception state: [POWER9 Processor User's Manual](https://wiki.raptorcs.com/w/images/c/ce/POWER9_um_OpenPOWER_v21_10OCT2019_pub.pdf).

## Authenticated Linux state, source provenance, and result

The standalone replay evidence is under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase31\run-1793921016091\linux-replay`; the final CTest replay independently produced a second two-VM evidence set under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase31\run-1793922143108`. The final focused CTest evidence is under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase31\unit-1793922142936`. The artifact verifier matched the authenticated kernel and config hashes, and an independent ELF inspector decoded the symbol and relative `__bug_table` entry without using the production PPC64 decoder as its authority.

Immediately before TDI, the authentic state was:

- NIP `0xC000000001EAA2C8`, bytes `0B 09 00 00`, word `0x0B090000`.
- R9 `0`; MSR `0x8000000000000002`; CR `0x44000000`; XER `0`.
- LR `0xC000000001EAA5B8`; CTR `0xC000000001EAA570`; SRR0/SRR1 `0`.
- Fetch was execute-permitted through guest-real address `0x01EAA2C8` to the same backing address. The last retired instruction was `rlwinm r9,r9,17,31,31` at `...A2C4`.

The last writer is `mfmsr r9` at `...A2C0`. The following `rlwinm` extracts MSR[EE] into R9; the captured MSR has EE clear (`0x8000` mask), so R9 is correctly zero. The authenticated ELF's own `__bug_table` maps this exact instruction to `arch/powerpc/kernel/setup_64.c:196` with flags `0x0901` (`WARN_ON`). Upstream Linux v7.2 contains the corresponding `WARN_ON(mfmsr() & MSR_EE)` in `fixup_boot_paca`: [Linux v7.2 `setup_64.c`](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/setup_64.c). Thus the assertion-site relationship is established by both binary data flow and the ELF's bug-table record, not inferred from the source alone.

TO=24 selects signed less-than OR signed greater-than, so zero does not trap. Production `VirtualMachine::step()` retired the authentic instruction, advanced NIP to `0xC000000001EAA2CC`, incremented cycles/timebase once, and preserved all GPRs, CR, XER, LR, CTR, MSR, SRR0/SRR1, and guest RAM. SRR0/SRR1 remained zero. The `blr` at `...A2CC` returned to `early_setup+0x48` at `...A5B8`; `addic r9,r13,-1` retired there. Linux then stopped before opcode 31 at NIP `0xC000000001EAA5BC`, `early_setup+0x4c`, raw word `0x7D296910`.

Two fresh 128 MiB LPID-1 production VMs replayed from authenticated Linux entry and agreed on the pre-TDI and post-TDI CPU states, continuation traces, final registers, guest-RAM fingerprints, and frontier. Each retired 2,641,058 instructions before TDI, retired TDI without a trap, then retired two more instructions: **2,641,061 total Linux instructions retired** at the new frontier. The Linux replay suite passed **116 checks** with zero failures.

The prior data-walk proof remains intact: 231,806 relocation records and CTR branches completed, the pointer advanced to `0xC000000002578030`, and relocation returned through LR. The existing eight subsequent permission-checked stores account for all 16 changed RAM bytes with zero unattributed bytes. The FDT backing remained unchanged. TDI itself changed no guest RAM.

## Regression results, limits, and handoff

- The rebuilt PPC64 Phase 1–31 targets, VM checkpoint tests, decoder/instruction and translation/exception suites, SAL tests, and console tests passed in the final CTest run.
- Full CTest contained 46 entries: **40 passed, 0 executed failures, 6 Not Run**. The unavailable executables were `test_syscall_dispatcher.exe`, `test_debugger.exe`, `test_profiler.exe`, `test_fuzzer.exe`, `test_vm_isolation.exe`, and `test_elf_validation.exe`. They are not counted as passing; CTest returned nonzero because those six executables were unavailable.
- Standalone IA-64 ACPI, interrupt-controller, and processor-interrupt-block checks all passed. `.phase` remained unchanged.
- `git diff --check` passed before closeout. The Phase 8 and Phase 19 regression expectations now reflect Book III's MSR[SF]=1 exception-entry state; Phase 30's lookahead count is 21 instructions from the RLDIMI frontier to the unsupported TDI.
- QEMU status remains `QEMU_REFERENCE_PENDING`; no QEMU runtime comparison was performed. This run does not claim full Linux boot, scheduler operation, or userspace execution.
- Recommended PPC64-32 scope: independently decode and qualify opcode `0x7D296910` at `early_setup+0x4c`, then continue from that authenticated frontier without changing the Linux ELF or its machine state by special case.
- Final Phase 31 implementation commit SHA: `9c9aed6a95cd4e7314d22e2faf899641464ba8ff`. The documentation closeout commit follows it; the final HEAD SHA is reported in the task closeout.
- Final tracked/untracked worktree: clean after the local closeout commits. Push status: **not pushed**.

The first full CTest attempt and the passing rerun are preserved as `ctest-full.log` and `ctest-full-rerun.log` in `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase31\run-1793921016091`. The primary run directory also contains the focused and authentic replay logs; both replay artifact directories include SHA-256 manifests.
