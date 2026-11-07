# PPC64 Phase 33 — SPRG1/PACA Linux Frontier

## Result

Phase 33 implements the guest-visible POWER9 SPRG1 register (SPR 273), tests its supervisor privilege rules and full-width read/write behavior, preserves it in CPU state and checkpoints, and retires the authenticated Linux mtspr instruction at the Phase32 frontier. Two fresh Linux replays agree through the next unsupported instruction. Linux did not reach a complete boot.

## Starting repository state

- Repository: D:\dev\guideXOS_Hypervisor_PPC64
- Branch: PPC64, tracking origin/PPC64
- Starting HEAD: 4a9aec9c1d3aaffef75d99bb87aa1e75e9b914c0
- Ahead/behind: 0/0 at preflight; local and upstream HEADs matched.
- Tracked and untracked worktree: clean at preflight.
- The ignored build/ directory was preserved.
- .phase-ppc64 recorded last_completed=PPC64-32 and next_expected=PPC64-33; the shared IA-64 .phase was not modified.

The Phase32 report and accepted artifacts were reviewed. Its authenticated frontier was NIP=0xC000000000034D48, decoded as mtspr 273,r3, after 2,641,070 retired instructions. The actual raw instruction and R3 value were not supplied by that report, so both were captured from fresh production VM execution below.

## Implementation

The existing XFX decoder already reconstructed the SPR number by reversing its five-bit fields. Phase33 adds SPRG1 to the existing SPR dispatcher and PPC64ISAState; it does not create another SPR implementation or hardcode a Linux address. Successful mtspr copies the complete 64-bit source GPR value to the per-vCPU SPRG1 field, and mfspr reads that field back. GPR0 remains an ordinary operand for these instructions.

SPRG1 is supervisor accessible. The existing privilege gate rejects problem state reads and writes with a Program exception at vector 0x700, SRR1_PROGPRIV, and the expected SRR/MSR state. The production guest handler skips each rejected instruction and returns with rfid; the unauthorized read leaves its destination unchanged and the unauthorized write leaves SPRG1 unchanged. Supervisor access with both PR and HV clear succeeds. HV=1 access also succeeds through the already configured hypervisor context.

Reset initializes SPRG1 to zero in this CPU model. VM A and VM B retain independent values. SPRG1 stays with the vCPU across LPID 1/2 context changes; the implementation does not add LPID banking. Pause/resume preserves the value.

The CPU payload advances to version 6 and appends one 64-bit SPRG1 value to the v5 layout. Existing v1-v5 payload readers remain supported and restore SPRG1 as zero. The full VMCheckpointCodec round trip restores SPRG1 and continues guest execution after resume. The outer VM checkpoint format was not revised.

SPRG0, SPRG2, SPRG3, and HSPRG0 (SPR 304) remain unsupported. The tests also verify that writing PVR does not change its value, PVR reads still return the POWER9 DD2.0 value 0x004E1200, and SPRG1 does not alias HSPRG0. Existing XER/LR/CTR and other SPR paths are covered by the earlier phase regression suite.

## Encoding and focused behavior

The independently encoded and actually fetched instruction is 0x7C7143A6:

| Field | Value |
|---|---:|
| Primary opcode | 31 |
| RS | 3 |
| Encoded low five SPR bits | 17 |
| Encoded high five SPR bits | 8 |
| Reconstructed SPR | (8 << 5) OR 17 = 273 |
| XO | 467 |
| Rc/reserved low bit | 0 |

The focused suite covers zero, one, all ones, mixed high/low halves, kernel effective addresses, and bit-63 values; multiple writes; intervening ordinary instructions; readback; RS/RT values 0, 3, 5, and 31; supervisor/problem-state access; exception recovery; reset; VM/LPID ownership; pause/resume; and checkpoint compatibility. Successful access preserves the other GPRs, CR, XER, MSR, LR, CTR, RAM, and MMU state.

The latest full-suite Phase33 run completed 954 checks (894 focused checks and 58 Linux replay checks, with zero failures). The earlier isolated Phase33 run completed 879 checks; both runs agreed on the authentic Linux results below. Phase32's refreshed authentic regression completed 613 checks, including 432 focused production VM-step checks, with zero failures.

## Linux provenance and authentic execution

The unchanged authenticated Debian kernel is vmlinuz-7.2.14+deb14-powerpc64. Its SHA-256 is c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd; the matching configuration hash is 592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa; and the reference FDT hash is 780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37. The committed artifact verifier passed.

The authenticated ELF places setup_paca at 0xC000000000034D30; the frontier at 0xC000000000034D48 is setup_paca+0x18. The preceding branch from early_setup reaches that symbol, and the function's earlier or r13,r3,r3 copies R3 into R13. The ELF's boot_paca.5 object starts at 0xC000000001F42380 (size 0x500), matching the captured R3 and R13 values. This supports attribution to setup_paca and the pointer's ELF provenance, rather than inferring source from a mnemonic. Linux v7.2 defines SPRN_SPRG1 as 0x111, aliases SPRN_SPRG_PACA to it, and writes the local PACA pointer to that SPR in the normal non-HV path. See [Linux register definitions](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/include/asm/reg.h), [PACA setup](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/paca.c), and [early CPU setup](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/setup_64.c). The pinned [QEMU SPR registrations](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/helper_regs.c) were used only as a source-level privilege cross-check; QEMU was not run.

At the reproduced frontier, both fresh VMs had:

| State | Captured value |
|---|---:|
| Retired prefix | 2,641,070 |
| NIP | 0xC000000000034D48 |
| Fetched word / decode | 0x7C7143A6 / mtspr 273,r3 |
| R3 | 0xC000000001F42380 |
| R13 | 0xC000000001F42380 |
| SPRG1 before | 0x0000000000000000 |
| MSR / LPID | 0x8000000000000002 / 1 |
| CR / XER | 0x24000000 / 0x00000000 |
| SPRG1 after | 0xC000000001F42380 |
| NIP after | 0xC000000000034D4C |

The instruction retired normally through VirtualMachine::step(). CR, XER, MSR, guest RAM, and FDT were unchanged by that instruction. The pointer maps to guest-real backing 0x01F42380; the first 32 bytes there have hash 0x4b159235c53eb273. This records provenance and mapping only; it does not validate a PACA C structure, and the SPR write itself performs no dereference.

## Bounded continuation and repeatability

After the old frontier, Linux retired another 10,284 instructions. The replay stopped before executing the unsupported instruction at NIP=0xC00000000138A414, raw word 0x7CA507B4; the primary opcode field is 31, and the current decoder reports <unsupported ppc64 opcode 31>. This is an unsupported-instruction endpoint, not a budget endpoint or a reported architectural exception. Total retired instructions were 2,651,354. SPRG1 remained 0xC000000001F42380; no subsequent mfspr SPRG1 was observed before the stop.

The two fresh runs agree on the 2,641,070-instruction prefix, R3, SPRG1 before and after, post-instruction NIP, 10,284-instruction continuation, stop NIP and raw word, trace digest 0x1b39a0c45a5d281f, final register state, and final guest RAM fingerprint. The kernel and FDT were not modified.

## Programs A–N

| Program | Evidence |
|---|---|
| A. Authentic encoding | Actual Linux fetch is 0x7C7143A6; fields independently reconstruct SPR 273. |
| B. Reversed SPR fields and source registers | Decoder and production VM-step tests cover the field order and RS 0/3/31. |
| C. Full-width writes | Zero, one, all-ones, mixed-width, kernel-style, and bit-63 values pass. |
| D. MFSPR readback | Guest readback after multiple writes and intervening instructions passes. |
| E. Supervisor access | PR=0, HV=0 reads and writes pass. |
| F. Privilege faults | PR=1 reads/writes raise Program privilege faults and recover through guest handler. |
| G. SPR isolation | PVR remains read-only; SPRG0/2/3 and HSPRG0 remain unsupported. |
| H. Architectural preservation | Successful access preserves unrelated GPRs, CR/XER/MSR/LR/CTR, RAM, and MMU state. |
| I. Reset and lifecycle | Reset-to-zero, independent VMs, LPID transition, and pause/resume pass. |
| J. Checkpoints | Version 6 round trip and continued execution pass; legacy v1-v5 CPU payloads read. |
| K. VM and LPID ownership | Register state is per-vCPU, independent per VM, and not newly banked per LPID. |
| L. Production execution | Actual mtspr/mfspr words execute through VirtualMachine::step(). |
| M. Authenticated Linux replay | The real setup_paca+0x18 write retires and stores the captured R3 pointer. |
| N. New frontier and regression | Two runs continue 10,284 retirements and agree at the later unsupported opcode. |

## Regression and artifact record

Previous accepted counts were 477,654 checks for PPC64 Phases 1–31 and 599 for the earlier Phase32 report (nominal sum 478,253). The refreshed Phase32 source now reports 613 checks; Phase33 reports 879. These measured source counts are reported separately rather than forcing the nominal historical sum.

The latest Phase32 run artifacts are under E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase32\run-1794064820682\; its manifest SHA-256 is 48698e16adba5753e1473a1538b56467342b4c1f22daa98a4c08f40a6d3a8d1d. The authoritative Phase33 artifacts, including the authentic state, retirement and continuation traces, checkpoint, two-run comparison, focused evidence, source-symbol inspection, full CTest log, standalone regressions, and rebuilt focused-test log, are under E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase33\run-1794065772080\. Its artifact-sha256-manifest.txt SHA-256 is d3b91c51ffe3579bbdd7ee081c5d7cd0e48878e9c872cc632a79a508aa959bed. Earlier full-suite and isolated runs are preserved in their respective run folders.

Full CTest ran all 48 registered tests: **42 passed, zero executed failures, and six Not Run** because their pre-existing executables are missing: test_syscall_dispatcher.exe, test_debugger.exe, test_profiler.exe, test_fuzzer.exe, test_vm_isolation.exe, and test_elf_validation.exe. Every available PPC64 phase test through Phase33, including the Phase31 Linux replay, passed. IA64 SAL also passed in CTest. The standalone IA64 ACPI firmware, interrupt controller, and Processor Interrupt Block executables pass. The verbose CTest log and standalone outputs are included in the authoritative Phase33 artifact directory.

## Limits and next phase

QEMU reference runtime status remains QEMU_REFERENCE_PENDING; no QEMU/QAC runtime was used. No Linux or FDT bytes were changed, and no PACA was fabricated. The guest has not completed Linux boot. SPRG0/2/3 and HSPRG0 remain unsupported. Phase34 should begin from the recorded unsupported opcode-31 frontier at 0xC00000000138A414, first independently identify its instruction and Linux source, and only then decide whether it falls within the accepted guest profile. Do not infer that the stored PACA pointer proves the PACA contents valid.
