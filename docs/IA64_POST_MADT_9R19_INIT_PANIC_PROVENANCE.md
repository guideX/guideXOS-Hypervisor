# IA64-POST-MADT-9R19 — Authentic Linux Init Panic Provenance

## 1. Outcome

**Outcome D — fault/register state localized; original cause remains open.**

The 0x4040 data fault is not a coincidental earlier diagnostic. Its fault handler enters Linux die(), which later calls do_exit(SIGSEGV) for the same current task. That task is the init_pid_ns.child_reaper; find_new_reaper() finds no eligible replacement thread and takes the exact init_pid_ns panic branch. The guest then executes panic("Attempted to kill init!").

The first suspicious state localized so far is in driver_register: its entry arguments are kernel addresses, but at driver_register+0x80 r14 is 0x4000, and the next load uses the resulting address 0x4040. The trace does not identify the instruction or memory write that changed r14, nor establish whether the bad value originated in guest code, emulator state, or an unsupported dependency. No production CPU or Linux behavior was changed.

The evidence rules out a separate host-generated panic and shows the precise guest exit chain. It does not justify declaring the low address a correct software pointer or an emulator defect.

## 2. Starting and ending Git state

- Started on branch IA64, at f64d51cce326ba8edba3fda9d6853c62ddca840d, one commit ahead of origin/IA64, with a clean tracked and untracked worktree.
- .phase began and remains:
  - project=guideXOS_Hypervisor
  - last_completed=IA64-POST-MADT-8
  - next_expected=IA64-POST-MADT-9
- The only source change is a diagnostic call in tools/ia64_iso_matrix.cpp so the existing guest-memory-range dump option also runs after checkpoint restore. The new report is the only other intended tracked change.
- Local commit and final cleanliness are recorded at the end of this report after commit. Nothing is pushed.
- All previous build/evidence directories and original guest artifacts remain in place.

## 3. R18 evidence inventory

R18 records the corrected interruption-frame implementation, the authenticated R16 checkpoint continuation, call-link/cover/RFI traces, the first symbol probes, the 1B-step replay, and the unresolved 0x4040 fault. Relevant evidence is under build-9r18/evidence/, including r18-1b-command.ps1, r18-fault-focused-progress.txt, and the bounded call and interruption logs. The full findings and regression history remain in docs/IA64_POST_MADT_9R18_INTERRUPT_FRAME_LINUX_PROGRESS.md.

R18’s first-hit IA64-DOEXIT record is not the panic-producing do_exit: it reports a worker task at r13=0xe000000008ec8000, entered from kthread+0x140. R19 distinguishes that event from the later die continuation for the panic-producing task at r13=0xe000000008ec0000.

## 4. R17/R18 replay comparison

Both reports identify the same post-EBS R16 checkpoint at guest cycle 778,197,459, with SHA-256 B6C258887359E4E6A5B0BCFE355B86FD6CFC7A410D735F575BE6688348564B4B and manifest SHA-256 3B030AEA30F29C1C3518F7401B49C95802521F6AB183ED753CED14848990ACE6. Both used the same 512 MiB guest, the same authenticated q6 ISO and Linux/initrd hashes, default virtual-time configuration, and no post-checkpoint ELILO input injection.

R17 completed 900,000,000 successful VM steps/cycles and ended at guest cycle 1,678,197,459. Its log already contained the guest diagnostic text “Attempted to kill init!”, but its instrumentation did not establish the function/call path. R18 completed 1,000,000,000 steps/cycles, ended at 1,778,197,459, and its probes showed the guest panic call by cycle 838,051,314, only 59,853,855 cycles after the checkpoint. Thus the observed panic was well before both run limits. R17’s RUNNING / panic=no and R18’s same host-side terminal status describe the emulator’s state at the requested budget; they do not mean Linux booted successfully.

The counters are comparable here: the replay harness counts one successful VM step per guest cycle. “Unique instructions” and “unique bundles” are coverage counts, not retired instruction-slot counts. The R17 and R18 replay source checkouts and diagnostic instrumentation were not identical, so the reports do not establish a bit-identical determinism experiment. There is no R17/R18 guest-outcome discrepancy to explain: R17 had the panic text, and R18 established its guest origin. R19 independently reproduces the same checkpoint-relative failure chain.

R17’s interruption-frame repair remains supported. Its earlier RFI zero-target obstruction is a distinct event and is not the interruption shown returning to die in R19.

## 5. Exact panic call

The matching Debian 3.2.78-1 System.map places do_exit at 0xa0000001000929a0 and panic at 0xa00000010081ac70.

At guest cycle **838,051,311**, do_exit+0x170, slot 2 executes:

    br.call b0 = 0x481ac70

The canonical target is 0xa00000010081ac70, the guest Linux panic() entry. Its first traced instruction executes at cycle **838,051,312**. The bounded restore trace records the format string “Attempted to kill init!” and reports the guest caller as do_exit+0x180. These are guest instruction and format-string observations; the harness terminal field remains state=RUNNING panic=no because the VM continues in Linux’s delay loop.

## 6. find_new_reaper() predicate

The matching Linux source is Debian’s Linux 3.2.78-1 source tree at E:/CodexArtifacts/guideXOS_Hypervisor/src-linux-3.2.78/linux-3.2.78. Its kernel/exit.c implementation obtains pid_ns = task_active_pid_ns(father), walks the task’s thread group, skips threads with PF_EXITING, and returns the first remaining thread. If the search ends without a replacement and pid_ns->child_reaper == father, it unlocks the task list and panics when pid_ns == &init_pid_ns.

R19’s instructions at do_exit dynamically establish the executed predicate:

- task_active_pid_ns() returns 0xa000000100c830d0, the init_pid_ns symbol.
- The current father is 0xe000000008ec0000.
- The live load of init_pid_ns.child_reaper returns 0xe000000008ec0000, equal to father.
- The executed comparison against &init_pid_ns is true.
- Control proceeds to the panic() call at cycle 838,051,311.

That path is reached only after find_new_reaper()’s replacement-thread search returns without an eligible thread; the matching source’s eligibility test is !(thread->flags & PF_EXITING). The bounded trace proves the actual namespace/reaper comparisons and panic branch, rather than inferring them from the panic text.

## 7. Task and namespace evidence

The current task is r13=0xe000000008ec0000 throughout the fault-to-exit path. Its guest stack is in the same direct-map allocation: at do_exit entry, r12=0xe000000008ec7b90. The bounded RAM capture independently covers the task address and the namespace object. It shows:

- the task stack field is 0xe000000008ec0c70;
- task state is 0 and exit state is 0 in the final capture, while the task remains in the panic loop;
- task flags are 0x10200044, including PF_EXITING (0x4) and PF_KTHREAD (0x00200000);
- exit code is 11 (SIGSEGV);
- parent and real-parent pointers resolve to the init_task symbol;
- comm contains “swapper/0”;
- init_pid_ns.child_reaper contains 0xe000000008ec0000.

The live r13, valid stack, independently executed child-reaper load, and saved guest RAM establish that the task pointer is not accepted merely because it resembles a kernel address. The namespace source and executed loads independently confirm the child_reaper field access at namespace offset 0x210. The exact matching source and config were inspected (CONFIG_SMP=y, CONFIG_PID_NS=y, CONFIG_IA64_PAGE_SIZE_16KB, CONFIG_BASE_SMALL=0).

The numeric task_struct.pid and tgid fields were not independently decoded. The available guest package has no unstripped vmlinux/DWARF layout, and no IA-64 cross compiler is installed. Accordingly this report does not invent numeric IDs or identify the task by comm alone. The task is identified by its live current pointer and its proven role as the init namespace’s child reaper. Task field offsets used for the RAM interpretation were checked against the matching source/config; the runtime namespace access was also confirmed in executed code. A full compiler-generated task-layout cross-check remains open.

## 8. First exit-path entry and signal

This is not the earlier worker kthread exit, do_group_exit, or a userspace return. The panic-producing chain begins with the kernel page fault described below, enters ia64_do_page_fault, calls die(), and then calls do_exit(SIGSEGV) for the same r13.

At cycle 838,048,130, the die continuation sets r40 to 0xb (signal 11). At cycle 838,048,131, slot 2 directly calls do_exit at 0xa0000001000929a0. At the later panic call, do_exit’s frame has r12=0xe000000008ec7b90, r13=0xe000000008ec0000, CFM/PFS 0x916, RSC 3, BSP 0xe000000008ec0ea8, and BSPSTORE 0xe000000008ec0e68.

## 9. Faulting instruction and exception classification

The exact fault is at guest cycle **837,327,815**, canonical IP 0xa000000100580f30 (driver_register+0x90), slot 1. The executed instruction is a predicated-true ld8 r3=[r8]; r8 is 0x4040. Immediately before it, driver_register+0x80 executes add r8=r14+0x40 with r14=0x4000, producing the effective address 0x4040.

The interruption snapshot records CR.IFA=0x4040, CR.ISR=0x400000000 (load access), CR.IIP=0xa000000100580f30, saved IPSR 0x1210085a6008, and ITIR 0x38. This is a real guest load generating a data page fault; it is not a host diagnostic access or an inferred null-derived pointer. The trace shows entry into ia64_do_page_fault at cycle 837,328,030.

The page-fault path calls die() at cycle 837,356,967. The first die() entry follows the same saved IIP and IFA, and the subsequent do_exit(SIGSEGV) and panic belong to that same current task and die frame. Therefore **the 0x4040 fault caused the task-exit path and the init panic**.

## 10. Pointer provenance and earliest localized bad state

A symbol probe at driver_register entry, cycle 837,299,012, sees argument r32=0xa000000100c6d300 and r14=0xa000000100c6d308, both kernel addresses. At driver_register+0x80, 28,800 guest cycles later, r14 is 0x4000. The next instruction adds 0x40, and the faulting load uses 0x4040.

This establishes the first localized invalid pointer base in the captured function window and shows it was not already present in r14 at function entry. The trace does not include every intervening write to r14 or prove which code was required to preserve that register value. The earliest incorrect transition is therefore localized to “r14 has become 0x4000 by driver_register+0x80”; its writer and ownership are unknown. That is enough for Outcome D, but not enough to attribute the transition to the emulator, classify it as a task-switch error, or implement a safe correction.

## 11. RSE and interruption-frame findings

The original fault is a register-based memory access in slot 1. The exception frame records the interrupted IP/IFA and the handler’s RSE state; the early fault handler reports CFM 0x50e, PFS 0x3, BSP 0xe000000008ec0e18, and BSPSTORE 0xe000000008ec0da8. These values are from the exception-handler frame, not substituted for the faulting function’s pre-exception register state.

During the long die() execution, a timer interrupt occurs. At cycle 838,048,110, the handler’s RFI restores CFM/PFS 0x40e, RSC 3, and BSP/BSPSTORE 0xe000000008ec0ed8; cycle 838,048,111 resumes at the saved die+0x270 address. The saved IFA remains 0x4040 but no new 0x4040 load occurs there: this is a timer interruption and return to the existing die frame. Die then continues to set SIGSEGV and call do_exit.

This RFI is within the panic-producing chain and was captured across its return. It restores the die frame consistently. It is not the earlier R17 zero-CFM return obstruction, and it gives no evidence that R17’s repaired interrupt-frame behavior regressed. R19 does not establish an actual task switch before this fault, so it does not claim task-switch ownership corruption.

## 12. Instrumentation changes

The only production-tree source edit is in tools/ia64_iso_matrix.cpp: checkpoint restore now calls the existing dumpGuestMemoryRange() helper with the configured range and output path, returning failure if the dump fails. This makes the existing read-only memory capture option available on checkpoint restores. It does not change guest memory, CPU state, instruction semantics, Linux, or checkpoint format.

The R19 traces are bounded: a 6,000-instruction panic/exit window; a 6,000-instruction early-die window; a 300-instruction faulting-instruction window; and a 500-instruction driver_register entry window. The restore continuation is capped at 60,000,000 guest cycles. No unbounded delay-loop trace was added.

## 13. Authentic replay and checkpoint qualification

R19 restored the same current-format R16 checkpoint used by R17/R18: cycle 778,197,459; checkpoint SHA-256 B6C258887359E4E6A5B0BCFE355B86FD6CFC7A410D735F575BE6688348564B4B; manifest SHA-256 3B030AEA30F29C1C3518F7401B49C95802521F6AB183ED753CED14848990ACE6. It used the matching 512 MiB guest, ISO SHA-256 6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E, LINUX.gz SHA-256 F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C, and INITRD.GZ SHA-256 D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B.

The R19 checkpoint continuations each ran 60,000,000 steps/cycles, recorded zero failed VM steps, and ended at guest cycle 838,197,459. The panic trace recorded 102,821 unique instructions and 34,545 unique bundles. The separately captured pointer-entry replay used the same checkpoint and budget. The panic branch and fault chain reproduce the R18 evidence; R19 adds the exact memory operation, source register history through function entry, live namespace/reaper values, and task RAM.

No new checkpoint was created. The authenticated existing checkpoint and guest hashes were verified before restore. The run is a fresh VM restore continuation, not a new checkpoint codec or uninterrupted-versus-restored equivalence qualification.

## 14. Linux milestone and scheduler frontier

No later Linux initialization milestone was reached. The guest enters panic() and remains in ia64_itc_udelay; that loop is not successful boot progress. Existing probes show execution through schedule/__schedule, but R19 does not capture a completed, independently identified task switch or qualify Linux scheduler/context-switch correctness. Phase 9 remains open.

## 15. Regression qualification

R19 uses a Release build under build-9r19. Results below are filled from this run’s focused IA-64 targets. The assertion-enabled R18 runs remain documented in the R18 report; they were built with -O3 -UNDEBUG, so checks were active. Bootstrap’s Windows Winsock link limitation remains outside this diagnostic-only change.

| Requested coverage | R19 result |
|---|---|
| test_new_instructions / NewInstructionTests | PASS, Release CTest |
| test_ia64_processor_interrupt_block | PASS, Release direct run |
| test_ia64_interrupt_controller | PASS, Release direct run |
| test_vm_checkpoint / VMCheckpointTests | PASS, Release CTest |
| test_ia64_single_cpu_context_switch | PASS, Release direct run |
| test_ia64_rse_automatic_pressure / IA64AutomaticRSEPressureTests | PASS, Release CTest |
| R7 checkpoint continuation | PASS, Release CTest |
| R8 RSE fault/restart | PASS, Release CTest |
| R18 call-frame/cover/RFI target | PASS, IA64CallFrameReturnTests Release CTest; interrupt-block coverage also passed |
| Full test_isa_plugin | PASS, Release direct run |
| Focused ISA SHL | PASS, Release direct run |
| Focused ISA NaT/state serialization | PASS, Release direct run |
| git diff --check | PASS |

The six Release CTest cases passed: NewInstructionTests, VMCheckpointTests, IA64CheckpointContextContinuationTests, IA64RSEFaultRestartTests, IA64CallFrameReturnTests, and IA64AutomaticRSEPressureTests. Direct Release outputs are preserved as build-9r19/evidence/r19-test-*.txt. The R19 changes do not alter the ISA, interrupt, RSE, VM checkpoint, or test sources. The accepted assertion-enabled R18 suite and its -O3 -UNDEBUG configuration remain documented in the R18 report.

The Windows test_bootstrap target is still known to be unable to link because the available MinGW target lacks Winsock import libraries. This is not counted as a passing test.

## 16. Remaining cause and R20 scope

R20 should capture the writes and call/register transitions that change r14 from the valid driver-register entry value to 0x4000, with RSE ownership, CFM/PFS, BSP/BSPSTORE/RNAT/NaT, branch links, and memory operands at those writes. It should compare the observed register lifetime against the exact compiled Debian driver-registration call path and IA-64 ABI, then test the smallest implicated architectural behavior if the trace establishes an emulator defect. It should first add a narrowly bounded writer-history replay and a matching independent task-layout check. It should not adjust task state, suppress the exception, or declare the task’s numeric PID/TGID without verified field offsets.

The page-fault handler and die frame have now been connected to the panic path, so R20 need not repeat broad panic-loop or interruption redesign work. Continue only through the first proven bad register transition and then, if justified, through a trustworthy Linux milestone.

## 17. Commit and final worktree

This report is committed locally together with the checkpoint-range diagnostic change. The full commit SHA and final repository status are recorded in the closeout response; embedding the commit’s own SHA inside this file would change that SHA. No push was made.
