# PPC64 Phase 33 — SPRG1 and authentic Linux PACA continuation

## Outcome

**Outcome A2 — SPRG1 is qualified and Linux completed setup_paca.** The authentic Debian Linux instruction at setup_paca+0x18 retired through the production VM step path, storing the live 64-bit r3 value in CPU-local SPRG1. Linux then returned from setup_paca to early_setup and retired another 10,284 instructions before reaching a new unsupported instruction: extsw r5,r5 at fdt_path_offset+0x44. This accepts Phase33; it does not claim a complete Linux boot.

## Repository state and phase marker

- Repository: D:\dev\guideXOS_Hypervisor_PPC64
- Branch: PPC64
- Preflight HEAD: 4a9aec9c1d3aaffef75d99bb87aa1e75e9b914c0, docs(PPC64): record final Phase 32 history.
- The request named 6eb93ffaeb54d186e6d0408437c7bfbdf624bb92 as its expected starting point. The actual clean checkout was at the newer 4a9aec9 commit, which includes the accepted later Phase32 documentation history. No newer work was discarded.
- At preflight the branch matched origin/PPC64, with zero ahead and zero behind. The tracked and untracked worktree was clean.
- The ignored build directory was present and preserved.
- At the Phase33 implementation commit, the marker was advanced to last_completed=PPC64-33 and next_expected=PPC64-34. The IA64 phase marker was not changed.
- Validated implementation commit: c7a1d90a4cca75d1d6384805b0ffc9d3f3e14e46. The follow-up documentation closeout is committed locally after it; no push was made.

## Inputs and preserved Phase32 evidence

The accepted Phase32 bundle remains at E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase32\run-1793938441052. Its 33 manifest entries were independently rehashed with zero mismatches; the manifest SHA-256 is e46349aa501a945cccf6d88dec7ebf8b7d0f87cfb22bada76c932031c32454d5.

The refreshed Phase32 CTest run is at E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase32\run-1794066857739. Its two fresh VM runs report 614 checks, zero failures: 432 focused VM-step checks, 180 authentic Linux replay checks, and two setup/artifact checks. Its 26 manifest entries were rehashed with zero mismatches; the manifest SHA-256 is C5C74EAD676FE779411B1D8BA6F2FC9E2EB663D970789CC49A5110BF2FE2FA78.

The authentic input remains Debian vmlinuz-7.2.14+deb14-powerpc64, an ELF64 big-endian PPC64 ET_EXEC image of 49,045,944 bytes with SHA-256 c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd. The kernel config SHA-256 is 592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa. The reference FDT is E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase28\ctest-final\run-1793798021385\entry-fdt.dtb, SHA-256 780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37. Both replays use the POWER9 pseries foundation, one vCPU, LPID 1, 128 MiB guest RAM, and direct-FDT entry; this profile does not use a separate firmware image.

## Instruction decode and architectural behavior

An independent ELF64 big-endian reader locates setup_paca and its unique SPRG1 write from symbol and instruction contents. The word fetched by the guest and independently checked is 0x7C7143A6:

| Field | Value |
|---|---:|
| Primary opcode | 31 |
| XFX source RS | r3 |
| Encoded low SPR field | 17 |
| Encoded high SPR field | 8 |
| Architectural SPR | (8 << 5) OR 17 = 273, SPRG1 |
| Extended opcode | 467 |
| Rc / reserved low bit | 0 |

The two five-bit SPR halves are reversed in the instruction encoding; reconstructing them in architectural order gives SPR 273. The production decoder keeps MTSPR, MFSPR, MTLR, MTCTR, and MTXER distinct. The authentic instruction was not special-cased, and neither its address nor the guest PACA value is hardcoded in the test.

SPRG1 is a full-width, per-vCPU architectural field owned by PPC64ISAState. At supervisor privilege it accepts the complete 64-bit source GPR value and has a production MFSPR readback path. The successful operation leaves source GPRs, CR, XER, LR, CTR, MSR, SRR0/SRR1, and normal guest RAM unchanged and advances NIP by four bytes.

The selected POWER9 profile permits SPRG1 access with PR=0; both supervisor and configured hypervisor contexts pass the production path. With PR=1, SPRG1 reads and writes raise a Program privilege exception at vector 0x700 with the expected saved state. A rejected write does not change SPRG1; the guest handler recovers from the tested fault. SPRG0, SPRG2, SPRG3, HSPRG0, and HSPRG1 are distinct unsupported SPRs in this implementation and report INVALID_ENCODING; they are not aliased to SPRG1. This phase does not claim HSPRG semantics.

SPRG1 resets to zero for a new VM and stays with its vCPU across pause/resume and LPID context changes; it is not LPID-banked. The platform profile enforces one vCPU per VM, so an in-VM CPU0-versus-CPU1 test is unavailable. The focused tests instead create independent one-vCPU VM contexts, write different values, and verify isolation.

The CPU ISA payload advances from version 5 to version 6 by appending one 64-bit SPRG1 value. Versions 1–5 remain readable and initialize the new field to zero. Saving SPRG1 through a legacy layout is rejected rather than silently truncating it. The outer VM checkpoint format is unchanged. Version 6 save/restore into a fresh VM, pause/resume, and deterministic continued execution pass. Reset, PVR read-only behavior, LR/CTR/XER alias behavior, and explicit unsupported-SPR cases are covered.

References: [OpenPOWER ISA specifications](https://openpowerfoundation.org/specifications/isa/), [IBM MTSPR instruction](https://www.ibm.com/docs/en/aix/7.3.0?topic=set-mtspr-move-special-purpose-register-instruction), [POWER9 processor user’s manual](https://wiki.raptorcs.com/w/images/c/ce/POWER9_um_OpenPOWER_v21_10OCT2019_pub.pdf), [Linux v7.2 SPR definitions](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/include/asm/reg.h), [Linux v7.2 PACA setup](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/paca.c), and [Linux v7.2 early setup](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/setup_64.c).

## Authentic PACA state and retirement

The ELF places setup_paca at 0xC000000000034D30 with size 0x20. The install at setup_paca+0x18 is at 0xC000000000034D48. The ELF object boot_paca.5 begins at 0xC000000001F42380 and has size 0x500. Linux v7.2 defines SPRN_SPRG1 as 0x111, aliases SPRN_SPRG_PACA to it, and writes the local PACA pointer to that SPR in the non-hypervisor path.

The branch at setup_paca+0x10 is 0x41820008 and targets the SPRG1 write. At +0x14 the skipped word is an HSPRG0 write (0x7C704BA6). The runtime MSR has HV=0, so the guest took the branch over HSPRG0 and reached the SPRG1 instruction at +0x18.

Immediately before MTSPR, both fresh production VM runs captured:

| State | Value |
|---|---:|
| Retired instructions before MTSPR | 2,641,070 |
| NIP / raw instruction | 0xC000000000034D48 / 0x7C7143A6 |
| Decode | mtspr 273,r3 |
| r3 | 0xC000000001F42380 |
| r13 | 0xC000000001F42380 |
| SPRG1 before | 0x0000000000000000 |
| MSR / decoded PR,HV | 0x8000000000000002 / 0,0 |
| LPID | 1 |
| CR / XER | 0x24000000 / 0x00000000 |

The captured r3 matches the ELF boot_paca.5 object address; r13 independently had the same value at this point. The PACA pointer’s guest-real backing is 0x01F42380. The first 32 bytes at that backing have hash 0x4b159235c53eb273. This establishes pointer provenance and mapping without claiming a full PACA structure audit.

A single production VM step retired the fetched instruction. SPRG1 became 0xC000000001F42380, NIP advanced to 0xC000000000034D4C, and CR, XER, MSR, guest RAM, and FDT remained unchanged. The result file records retired_through_instruction=2641071 and confirms no ordinary RAM write by MTSPR.

Both clean VMs used the same ELF, config, FDT, one-vCPU profile, and initial state. They agree on the Phase32 relocation and arithmetic milestones, the TDI no-trap behavior, SUBFE result and carry state, r3/r13 provenance, SPRG1 before and after, retirement and next NIP, continuation, final CPU state, RAM fingerprint, and FDT integrity. The refreshed Phase32 replay confirms 231,806 relocations completed with a valid return path and no invalid relocation access or executable-content corruption.

## setup_paca completion and next frontier

Linux executed bclr 20,0 at 0xC000000000034D4C and returned to early_setup at 0xC000000001EAA5CC. It retired another 10,284 instructions, for 2,651,354 total, while SPRG1 remained 0xC000000001F42380. The two fresh runs agree on the same continuation count, next frontier, trace digest 0x1b39a0c45a5d281f, final register state, and guest RAM fingerprint. No subsequent MFSPR SPRG1 read occurred before the stop.

The next instruction is at 0xC00000000138A414, raw word 0x7CA507B4. Its fields are primary opcode 31, RS=5, RA=5, RB=0, XO=986, Rc=0. The independent field decode identifies extsw r5,r5. ELF symbol lookup places it at fdt_path_offset+0x44 (symbol address 0xC00000000138A3D0, size 0x50). The production decoder still reports this instruction as unsupported, and the guest stopped before it executed; no behavior is claimed for it. IBM’s instruction reference specifies XO 986 for EXTSW and the sign-extension operation. [IBM EXTSW instruction](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-extsw-extend-sign-word-instruction).

The prior Phase32 and Phase33 runs leave the authenticated kernel and FDT unchanged. SPRG1 itself causes no normal-memory writes. The full Phase33 trace, one-step retirement record, bounded continuation, CPU/checkpoint evidence, ELF inspection, and input hashes are preserved with the qualification logs under E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase33\run-1794065963908. That directory’s 21 manifest entries all rehash with zero mismatches; its artifact-sha256-manifest.txt SHA-256 is 77f0318388ee0bca67e89c625d85035ce55b8f0569e9b10d08cb10f8dc41d5f6.

## Regression results

| Qualification | Result |
|---|---|
| Phase33 focused and Linux replay | 954 total checks; 894 focused, 58 Linux replay, 2 setup/artifact, 0 failures |
| Refreshed Phase32 | 614 total checks; 432 focused, 180 Linux replay, 2 setup/artifact, 0 failures |
| Full serial CTest | 48 registered; 42 passed, 0 executed failures, 6 Not Run; 519.81 seconds |
| Phase16 isolated rerun | 1 CTest passed; 1,522 program checks, 0 failures |
| IA64 ACPI firmware | Passed all seven named checks |
| IA64 interrupt controller | Passed |
| IA64 Processor Interrupt Block | Passed |
| git diff --check | Passed |

CTest returned code 8 because CTest treats Not Run tests as an incomplete suite. These six executables are absent from the existing build and were not counted as passes: test_syscall_dispatcher.exe, test_debugger.exe, test_profiler.exe, test_fuzzer.exe, test_vm_isolation.exe, and test_elf_validation.exe.

An initial parallel CTest run showed one Phase16 exact-recovery failure while host pipe tests were concurrent; its isolated rerun passed, and the complete serial run passed Phase16 and all other available tests. The serial log is the final suite result. The missing executables remain Not Run, as in the Phase32 baseline.

The full CTest run covers PPC64 Phases1–33, Phase31 Linux replay, VM checkpoint, decoder/VM-step, exception, MMU/translation, relocation, IA64 SAL, and console smoke/recovery tests. The standalone IA64 outputs and both CTest logs are included in the Phase33 evidence directory above.

QEMU runtime/reference qualification remains QEMU_REFERENCE_PENDING. No QEMU/QAC runtime was used. The architecture review uses Power ISA, IBM, the POWER9 manual, Linux v7.2 sources, independently decoded authentic ELF words, and production VM-step tests.

## PPC64-34 scope and closeout

Start Phase34 at the recorded unsupported extsw r5,r5 at fdt_path_offset+0x44. Confirm the instruction’s profile and sign-extension/CR semantics against the selected ISA, implement only the required operation through the production decoder and VM-step path, and replay the authentic continuation in two fresh VMs. Preserve the existing relocation, RAM, FDT, checkpoint, and CPU-state evidence. Do not infer a Linux boot milestone from this instruction.

The Phase33 implementation and marker are committed locally on PPC64. The working tree is clean after the report-path closeout, the branch is two commits ahead and zero behind origin/PPC64, the ignored build directory and prior evidence were preserved, and nothing was pushed.
