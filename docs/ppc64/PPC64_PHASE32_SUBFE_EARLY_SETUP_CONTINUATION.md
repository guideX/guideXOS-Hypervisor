# PPC64 Phase 32 — SUBFE and authentic Linux early setup

**Outcome A1 — PPC64-32 accepted.** SUBFE now decodes and executes through the production VM step path with 64-bit carry, independent CA32, OE, Rc, and alias behavior. Two fresh authenticated Linux replays retired the observed instruction and advanced to the next genuine unsupported instruction. This phase does not claim that `early_setup` returned, that `setup_paca` completed, or that Linux booted.

## Repository and preserved Phase 31 evidence

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`, branch `PPC64`.
- Starting HEAD: `65fe490e82a28e94097fa6c36d36d7b6e7c59cee`, `docs(PPC64): record Phase 31 TDI frontier`. The requested `2628d0602c29f56dd75cc81566f821058782d2fe` is its ancestor; the checkout was at `origin/PPC64` with zero divergence. The initial tracked/untracked worktree was clean.
- Starting marker: `last_completed=PPC64-31`, `next_expected=PPC64-32`. The IA-64 `.phase` marker and architecture checkout were not changed.
- Phase 31 manifest: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase31\run-1793921016091\artifact-sha256-manifest.txt`. Its SHA-256 is `B827269FEF5122D4829F717E6423F5441F9E8A62551E439367B9F04AA44E06C1`; all 31 listed files independently hash-verified.
- Phase 31 authenticated kernel: Debian `linux-7.2.14-1`, ELF64 big-endian PPC64 ET_EXEC, 49,045,944 bytes, SHA-256 `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd`.
- Kernel config SHA-256: `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa`.
- FDT: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase28\ctest-final\run-1793798021385\entry-fdt.dtb`, SHA-256 `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37`.
- Both phases use the POWER9 pseries foundation profile, LPID 1, one CPU, 128 MiB guest RAM, and direct-FDT entry. There is no separate firmware image in this configuration.
- Phase 31's TDI run retired 2,641,061 instructions and stopped at `early_setup+0x4c`; its relocation, FDT, and memory accounting are preserved.

## Independent instruction decode and implementation

The original frontier is guest NIP `0xC000000001EAA5BC`, raw word `0x7D296910`. ELF symbol lookup places it at `early_setup+0x4c`. A separate ELF reader and Capstone 5.0.7 PPC64 big-endian disassembly confirm the instruction and surrounding control flow; the production decoder is not the decode oracle.

| Field | Value |
|---|---:|
| Primary opcode | 31 |
| RT | 9 |
| RA | 9 |
| RB | 13 |
| OE | 0 |
| XO | 136 |
| Rc | 0 |

The word decodes as `subfe r9,r9,r13`. Power ISA defines the operation as `RT = NOT(RA) + RB + CAin`; equivalently, `RT = RB - RA - (1 - CAin)` modulo the active width. In 64-bit mode CA reports the 64-bit carry out; CA32 independently reports carry from the low 32-bit calculation. In 32-bit mode CA follows the low-word carry. The independent test oracle uses the subtraction form and `RB >= RA` / `RB > RA` no-borrow comparisons.

The implementation reads both source GPRs and CA before writing RT, computes 64-bit and low-word results separately, and updates CA/CA32. It supports and distinguishes `subfe`, `subfe.`, `subfeo`, and `subfeo.`. OE updates mode-width OV, low-word OV32, and sticky SO; Rc updates only CR0 using the active result width and SO. Rc=0 preserves CR, and OE=0 preserves OV/OV32/SO. Carry-producing arithmetic instructions also maintain CA32, which is required to preserve real carry provenance across `addic` and related operations. The existing XER checkpoint field remains 32 bits; the new flags fit its existing architectural XER[32:63] representation.

The focused suite exercises the exact word through `VirtualMachine::step()`, both carry-in values, independent arithmetic and carry boundaries, CA/CA32 differences, high-bit and wraparound cases, SF=0/1 behavior, all four OE/Rc forms, signed overflow and sticky SO, CR preservation, seven register alias arrangements, source ordering, PC and special-register preservation, no memory effects, instruction serialization, and neighboring arithmetic encodings. It also checks ADDIC/ADDC/ADDE/SUBFC carry updates and existing ADD/SUB/DIVD overflow flag behavior.

## Live carry provenance and source-level purpose

Immediately before SUBFE, both fresh VMs captured:

- NIP `0xC000000001EAA5BC`, instruction `0x7D296910`, `subfe r9,r9,r13`.
- `r9=0xFFFFFFFFFFFFFFFF`, `r13=0`.
- XER `0`: CA=0, CA32=0, OV=0, SO=0.
- CR `0x44000000`, LR `0xC000000001EAA5B8`, CTR `0xC000000001EAA570`.
- MSR `0x8000000000000002`, LPID 1.

The actual immediately preceding carry writer is `addic r9,r13,-1` at `0xC000000001EAA5B8` (word `0x312DFFFF`). It read `r13=0` and `r9=0`, produced `r9=0xFFFFFFFFFFFFFFFF`, CA=0, CA32=0, and returned to the SUBFE address. The two instructions are adjacent, so no instruction between them can alter CA. Phase 32 does not infer the carry from TDI.

For the live operands, `NOT(r9)=0`; adding `r13=0` and CAin=0 gives result 0, CAout=0, and CA32out=0. The production VM retired SUBFE, advanced NIP to `0xC000000001EAA5C0`, and preserved CR, OV, OV32, SO, LR, CTR, MSR, and guest RAM. The captured post-state has `r9=0`, XER=0, and CR `0x44000000`.

The authenticated ELF disassembly shows the full sequence:

```text
early_setup+0x48  addic r9,r13,-1
early_setup+0x4c  subfe r9,r9,r13
early_setup+0x50  tdi 24,r9,0
early_setup+0x54  or r3,r30,r30
early_setup+0x58  bl setup_paca
```

Thus SUBFE materializes the nonzero test of the original `r13`; the following `tdi 24,r9,0` tests that result. The actual zero result does not trap. This binary data flow attributes the code to the nearby `WARN_ON(local_paca)` in Linux v7.2 `early_setup`, rather than relying on source proximity. The authenticated Debian ELF's relative `__bug_table` maps that TDI to `arch/powerpc/kernel/setup_64.c:364` with flags `0x0901` (`WARN_ON`). Upstream Linux v7.2 source orders `fixup_boot_paca(&boot_paca)`, `WARN_ON(local_paca)`, then `setup_paca(&boot_paca)`.

References: [OpenPOWER ISA specifications](https://openpowerfoundation.org/specifications/isa/), [IBM SUBFE instruction](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-subfe-sfe-subtract-from-extended-instruction), [pinned GNU Binutils PPC opcode table](https://gnu.googlesource.com/binutils-gdb/+/37f7f684de0e53c8add5ff43d73082bf98985321/opcodes/ppc-opc.c), [Linux v7.2 `setup_64.c`](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/setup_64.c), and [Linux v7.2 `reg.h`](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/include/asm/reg.h).

## Two fresh Linux replays and next frontier

Two new VMs loaded the same authenticated ELF, config, and FDT and agreed on the initial state, relocation completion, TDI behavior, ADDIC carry provenance, pre/post-SUBFE state, and final frontier. Each completed all 231,806 relocation entries and CTR branches; the relocation pointer reached `0xC000000002578030`, and the return path completed. No invalid relocation access or executable-content corruption was observed. The loaded and backing FDT stayed equal to the authenticated reference. SUBFE itself and the subsequent 11-instruction continuation made no guest-RAM changes; the full-VM RAM fingerprint is recorded across the stop point.

After TDI, Linux retired 11 instructions, including SUBFE and the false warning check. Total guest retirements at the stop were **2,641,070**. Linux entered `setup_paca` and stopped before executing:

```text
NIP  0xC000000000034D48  setup_paca+0x18
word 0x7C7143A6          mtspr 273,r3
```

SPR 273 is `SPRN_SPRG1` in Linux v7.2 `arch/powerpc/include/asm/reg.h`. The code has copied the boot PACA pointer into r13 and branched around the EE-only SPR write, then reached the SPRG1 write. `setup_paca` did not return, so `early_setup` completion and later CPU/platform setup remain unobserved. Recommended PPC64-33 scope: define and test privileged `mtspr SPRG1` semantics for the POWER9 profile, then continue from this exact frontier while checking PACA register state and exception behavior.

At this frontier, the recorded state is R3=R13=`0xC000000001F42380`, R9=0, CR=`0x24000000`, XER=0, LR=`0xC000000001EAA5CC`, CTR=`0xC000000001EAA570`, MSR=`0x8000000000000002`, LPID 1, and SRR0/SRR1=0. The guest-RAM fingerprint is `0xD0B0364F64B366E4`, equal to its post-SUBFE value; FDT integrity remains true. The recent access log's last guest write is at `0xC000000001EAA2BC`, before the SUBFE site, and no exception was entered at the unsupported instruction.

## Evidence and regression results

Final Phase 32 replay artifacts are under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase32\run-1793938441052`. The directory includes the production replay trace, pre/post SUBFE state, carry-writer dataflow, independent Python ELF inspection, Capstone disassembly, FDT/kernel identities, memory-access log, frontier CPU state, full CTest and targeted CTest logs, and standalone IA-64 logs. Its SHA-256 manifest has 32 entries, all verified; manifest SHA-256 is `7f6595d5f24dcf82cf5999f9aed99caa27ca78bcbcdb6f3ea9089c7ca323fd2c`.

- Final Phase 32 target: **432 focused production VM-step checks**, **165 authentic Linux replay checks**, **0 failures**. Two fresh Linux VMs were used. Two setup/artifact checks bring the executable's total to 599.
- Full CTest: 47 entries, **41 passed, 0 executed failures, 6 Not Run**. The unavailable shared executables were `test_syscall_dispatcher.exe`, `test_debugger.exe`, `test_profiler.exe`, `test_fuzzer.exe`, `test_vm_isolation.exe`, and `test_elf_validation.exe`. CTest returned nonzero only because those six executables were unavailable; none is counted as a pass.
- Full CTest included PPC64 Phases 1–31, VM checkpoint, arithmetic/decoder, exception, translation/relocation, SAL, and console cases. The final Phase 32 target was rebuilt and rerun after the final evidence and stop-state assertions were added.
- Standalone IA-64 checks passed: ACPI firmware, interrupt controller, and processor interrupt block.
- `git diff --check` passed. QEMU differential qualification remains `QEMU_REFERENCE_PENDING`; QEMU was not launched.
- The marker advances to `last_completed=PPC64-32`, `next_expected=PPC64-33` only after the validated local commits. The implementation commit SHA, final worktree state, final HEAD, and push status are recorded at closeout.

## Limits

This accepts PPC64-32 only. It establishes SUBFE's architectural behavior and the authentic `early_setup` check through the new `setup_paca` frontier. It does not establish `setup_paca` completion, return from `early_setup`, additional CPU initialization, platform setup, QEMU equivalence, or full Linux boot.
