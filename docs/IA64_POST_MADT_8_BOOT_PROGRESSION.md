# IA-64 POST-MADT Phase 8 — Authentic Boot Progression Beyond RCU

**Outcome A (full progression, no repair required) with a diagnosed next
frontier.** The authentic Debian 3.2.78 IA-64 kernel completes `start_kernel`,
enters `rest_init`, spawns `kernel_init`/`kthreadd`, and `kernel_init`
progresses through `smp_prepare_cpus` into `do_pre_smp_initcalls` — far beyond
the Phase 7R1 RCU steady state — with no emulator repair. The run then parks
`kernel_init` in `kthread_create→wait_for_completion` while `kthreadd` reports
`scheduling while atomic` (preempt_count `0xC0000000`) ~323 times and finally
wedges in a ticket-lock spin. No guest panic, `do_exit`, or `die` occurs in
any bounded run. The Phase 7R1 RCU result remains intact.

## 1. Outcome classification

- **Outcome A — full progression (first prong).** New, previously unverified
  Linux initialization milestones are reached under valid architectural
  execution with no repair: `start_kernel` completion (full dmesg through
  ACPI), `rest_init` entry, `kernel_init` + `kthreadd` spawn and execution,
  `smp_prepare_cpus`, and entry into early-initcall processing. Evidence:
  deterministic multi-budget replays (100M/250M), hot-IP bundle accounting,
  last-hit register probes with ITC ordering, and the guest's own `log_buf`
  dmesg extracted from a 512 MB RAM dump.
- **No emulator repair was required or performed.** Production code, tests,
  and the phase gate are untouched by semantics changes (only this report is
  added; temporary diagnostics were fully reverted).
- **Next frontier diagnosed (first incorrect transitions established, no
  repair yet).** `kthreadd`'s `thread_info->preempt_count` reads `0xC0000000`
  in RAM (`PREEMPT_ACTIVE` bit 30 plus spare bit 31) from its first schedule,
  and a ticket lock at guest PA `0x8ec7dd8` wedges on one stale `fetchadd`
  read in ~52K balanced takes. Both writers are captured with cycle/IP/
  register/RAM evidence (§13); root-cause mechanism work is scoped for
  Phase 9 (§19). No panic, `do_exit`, or `die` occurs in any bounded run.
  The Phase 7R1 RCU result remains intact.

## 2. Starting and ending Git state

- Starting: branch `IA64`, HEAD `c82ebbbd70cd359e8ee995532b0dec3f3d8b1565`
  (`Fix IA-64 M6/M9 FP spill/fill decode ...`), `.phase`
  `last_completed=IA64-POST-MADT-7`, `next_expected=IA64-POST-MADT-8`,
  tracked worktree clean (`git diff HEAD --stat` empty); divergence
  `HEAD...origin/IA64` = `0 0`. Untracked pre-existing scratch `build-7r1/`
  preserved.
- Ending: same branch, one new local commit containing only this report and
  the `.phase` advance (`7→8` to `8→9`). `git diff --check` clean. All
  traces/logs/dumps under `C:\Users\guideX\AppData\Local\Temp\opencode\phase8\`
  (untracked scratch, never committed). No branches created/renamed, no
  merge/rebase/reset/clean/stash/worktree, no guest-binary or checkpoint
  writes. No remotes added/changed; no push attempted beyond reporting.

## 3. Verified artifact hashes

Verified via `Get-FileHash -Algorithm SHA256` before any runs (artifacts live
in the sibling checkout's `.codex_tmp`, restored read-only):

- EFI loader (378,664 bytes,
  `D:\dev\guideXOS_Hypervisor\.codex_tmp\bootia64_exact.efi`):
  `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA` ✓
- Decompressed Linux (13,035,024 bytes,
  `...\authentic-linux-decompressed.bin`):
  `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483` ✓
- Accepted checkpoint (574,577,483 bytes,
  `...\post_mux1_fixed_ebs.chk`):
  `71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B` ✓
- Guest identity passed verbatim to every restore:
  `debian-7.11-ia64;linux-size=13035024;linux-sha256=F2C573...;efi-size=378664;efi-sha256=D1AE6A...`
- Checkpoint manifest: `checkpoint_cycle=785426373`,
  `checkpoint_itc=0x2ed0a7c5`. Checkpoint re-verified byte-identical after
  all runs (read-only restore; no derived checkpoints written).

## 4. Phase 7R1 regression baseline (reproduced exactly)

100M-cycle bounded restore-continuation of `post_mux1_fixed_ebs.chk`
(`--memory-mib 512`, `build-7r1` binary built from HEAD `c82ebbb`):

| Metric | Phase 7R1 accepted | Phase 8 reproduction |
|---|---|---|
| Continuation budget | 100M cycles | 100M cycles |
| Executed steps (slots) | 69,251 instr / 23,123 bundles | 69,251 / 23,123 (identical) |
| Timer interrupts | 200 entries / 201 returns | 200 / 201 (identical) |
| RCU body executions | ~90,261 | 90,264 at `0x13e6a0/0x13e6c0` |
| RCU null-pointer faults | 0 | 0 |
| `r11` at `0x13e6c0`/slot2 | `0xa000000100cc5fc0` | identical (`itc=0x5ca10f9`, `r10` fresh) |
| `doExit` / panic / `die` | 0 / 0 / — | 0 / 0 / 0 |
| Host VM result | `RUNNING` | `RUNNING` |
| Final IP | `0x48245e0` | `0x48245e0` (identical) |
| Console | 2,840 bytes, `deltaBytes=0` | identical |

A 100K smoke run also reproduces exactly (276/109 → `0x47f8260`).

## 5. Continuation budgets and semantics

All runs are independent bounded restores of the accepted checkpoint
(`--checkpoint-read ... --post-checkpoint-cycles N`). Counts are
replay-relative steps (one `vm.step()` = one instruction slot); absolute
guest cycle = `785426373 + steps` (e.g. 100M → `885426373`, 250M →
`1035426373`, both confirmed in harness output). ITC (`ar44`) is the guest
timestamp clock and is used only for within-run ordering.

| Budget | Result |
|---|---|
| 100K | 276/109 → `0x47f8260`, RUNNING (matches 7R1) |
| 100M | 69,251/23,123 → `0x8245e0`, RUNNING, `doExit=0`, `panic=0` |
| 250M | 69,486/23,199 → `0x8298b0`, RUNNING, `doExit=0`, `panic=0` |
| 100M + 7 probes | identical counts/trajectory (deterministic) |
| 250M + 8 probes + hot-IPs | identical counts/trajectory (deterministic) |
| 250M + 512 MB RAM dump | identical trajectory (dump analyzed below) |

No repair was needed for any budget; all trajectories are bit-deterministic
across probe/hot-IP configurations.

## 6. Absolute vs replay-relative counts

- Checkpoint absolute base: cycle `785426373`, ITC `0x2ed0a7c5`.
- 100M final: absolute cycle `885426373` (base + 100,000,000).
- 250M final: absolute cycle `1035426373` (base + 250,000,000).
- Guest time (dmesg): `t=0.00` at `start_kernel` banner through `t≈1.30 s`
  at the last `scheduling while atomic` report (250M budget).

## 7. Linux initialization milestone timeline

All addresses canonical VMAs (`0xA000000100000000` base). Probe evidence is
last-hit register state with guest ITC; hot-IP evidence is executed-slot
counts (`--hot-ips`, one count per executed slot):

1. **Entry stub + 256 KB copy** (`0x7f8120–0x7f8300`, DTR/`itr.d`/RSE/`loadrs`
   setup, `memcpy` loop 32,768 iters at `0x7f8250/0x7f8260` ×3 slots). The
   checkpoint resumes mid-copy at `0x7f8260`; the copy completes once.
2. **`start_kernel` = `0xa00920`** entered once (alloc 20,15; 78 direct
   calls; tail-calls `rest_init` at `0xa011fc`). Full dmesg through ACPI
   (`Linux version 3.2.0-4-itanium ... Debian 3.2.78-1`, EFI/SAL/memory/RCU/
   IRQ/calibrate `49.08 BogoMIPS`/caches/cgroup/ACPI) proves completion.
3. **`rest_init` = `0x7f8310`** entered once. Body matches 3.2 source exactly
   (`rcu_scheduler_starting`, 2× `kernel_thread` with `CLONE_FS|SIGHAND`
   / `CLONE_FS|FILES`, `numa_default_policy`, `find_task_by_pid_ns`,
   `complete`, `init_idle_bootup_task`, `schedule`, `cpu_idle`).
4. **`kernel_init` = `0xa01220`** (resolved via `.got` descriptors; follows
   `start_kernel` in `.init.text`) entered once; **`kthreadd` = `0xc7cb0`**
   spawned (`kernel_thread` = 2 visits total) and scheduled (runs to its
   `schedule()` at `0xc7e9c`).
5. **`smp_prepare_cpus`** (`Boot processor id 0x0/0x0` in dmesg;
   kernel_init call-site `0xa0138c` executed).
6. **`do_pre_smp_initcalls`** (inlined loop at `0xa013f0`, array
   `[0xacd540, 0xacd570)` = 6 early-initcall descriptors in `.init.data`):
   entry[0] (`0xa12c30`) completes; entry[1] (`0xa14750`, CPU-hotplug
   bring-up: drives callback with actions 3 then 2, `BUG_ON`, registers
   notifier) proceeds into `cpu_callback` (`0x812300`) →
   `kthread_create` (`0xc7910`) → `notifier/try_to_wake` path (`0x76340` →
   `0x75c30`, lock acquired/released, notifier callbacks return) →
   **`wait_for_completion` (`0x8258e0`) — `kernel_init` parks here
   (NORMAL scheduler sleep, awaiting `kthreadd`).**
7. **NOT reached in any budget:** `smp_init`, `do_basic_setup`, `init_post`
   (`0x81a1f0`, 0 hits), `run_init_process` (`0x81a190`, 0 hits),
   `panic("No init found")`, root mount, userspace. `die` (`0x3a2d0`),
   `panic` (`0x81ac70`), `do_exit` (`0x929a0`) all 0 hits.

## 8. Highest verified authentic kernel milestone

**`kernel_init` parked in `kthread_create→wait_for_completion`, with
`kthreadd` alive and scheduling (guest time advancing to ~1.3 s).**
Concretely: the second early initcall's `kthread_create` for the boot CPU's
worker/`ksoftirqd` waits for `kthreadd` to process the create list, while
`kthreadd` itself runs but reports `scheduling while atomic` on every
schedule (~323 dmesg reports, `t=0.13–1.30 s`) and finally wedges in a
ticket-lock spin. This is steady-state multi-task Linux execution
(`kernel_init` + `kthreadd` + idle/RCU + timers), well past the Phase 7R1
RCU-only frontier, with no repair.

## 9. Guest task/context evidence

- `kernel_init` task: `current=0xe000000008ec0000`, comm `swapper/0`
  (init task, from RAM dump).
- `kthreadd` task: `current=0xe000000008ec8000`, comm `kthreadd`
  (task at PA `0x8ec8000`, `comm` at +`0x348`; thread_info at PA
  `0x8ec8c70`).
- Last-hit probes distinguish the tasks (`r13` differs) and order events by
  guest ITC (loop `0x2d6df88/8b` → `do_one_initcall` `0x2d6df8e` →
  RCU `0x5ca10f9` → lock spin `0xebae519`).
- `kernel_thread` = 2 visits (only rest_init's spawns; `kthreadd` never
  creates the worker thread).

## 10. Interrupt and exception behavior

- 100M: `timerCompareEvents=200`, `interruptEntries=200`,
  `interruptReturns=201`, `spurious=201(0xf)`, `blockedByPSR=1057867`.
- 250M: compares 329 / entries 328 / returns 329, `blockedByPSR=112918970`,
  final `pending=1`, `psrI=0` (spinner holds IRQs disabled at the wedge).
- Timer programming (`ITV/ITM`), first-firing ITC, EOI, and PIB
  (`acceptedINT=1`, vector `0xef`) all sane and deterministic across runs.
- No fault traces, no `die`/`do_exit`/panic in any window.

## 11. Evidence of forward progress

- Executed code spans `.text` AND `.init.text` (5,843 init bundles in 100M);
  `start_kernel`'s 78 calls all execute; scheduler runs 157K `schedule()`
  calls; RCU body ~90–158K executions scaling with budget.
- Guest dmesg advances from `t=0.00` to `t≈1.30 s` with hundreds of new
  kernel messages (init sequence + 323 `scheduling while atomic` reports).
- 100M→250M adds only 235 instructions / 76 bundles: the remaining work is
  steady-state task cycling plus, at the end, the ticket-lock wedge
  (18.6M spin hits at `0x829890/0x8298b0`, zero in 100M).

## 12. Next obstruction

Two linked guest-visible anomalies (same run, in order):

1. **`BUG: scheduling while atomic: kthreadd/2/0xc0000000`** (~323×,
   dmesg): `kthreadd`'s `thread_info->preempt_count` (guest PA `0x8ec8c98`)
   reads `0xC0000000` in RAM — IA-64 `PREEMPT_ACTIVE` (bit 30) plus spare
   bit 31 — from its first schedule. Neighbors (`exec_domain`, `flags`,
   `cpu`, `restart_block.fn`) are intact: a surgical 4-byte corruption.
   `kernel_init` (same ancestry) schedules cleanly, so this is not a
   global scheduler skew.
2. **Ticket-lock wedge:** `kthreadd→schedule→__schedule` spins at
   `0x829890` on guest PA `0x8ec7dd8` (`r32`), `r15(my ticket)=0x1003e`,
   `r11(serving)=0x1003f`, IRQs disabled. Per the IA-64 ticket protocol
   (`TICKET_SHIFT=17`, `TICKET_MASK=0x7fff`, verified against
   `arch/ia64/include/asm/spinlock.h`), 63 takes with zero serving advances
   cannot arise from balanced take/unlock pairing (which would grow the word
   by `0x20001`/cycle and keep the fast path viable — as seen in ~12K
   successful fast-path acquires elsewhere). Five neighboring words hold the
   same `0x1003e` stamp.

## 13. Exact instruction and causal chain (established)

An env-gated write-watch (`GUIDEXOS_WATCH_CORRUPTION`, temporary harness
diagnostic, reverted before commit) logged every guest store to the two
affected physical words across a full 250M-step replay (105,467 hits;
trajectory bit-identical to the uninstrumented baseline: 69,486/23,199).

### 13a. `preempt_count` corruption — unbalanced `PREEMPT_ACTIVE` sub

- Writer of the corrupting store: bundle `0xa00000010007c080` slot 0,
  `st4 [r14]=r32` (`preempt_schedule` exit path: `ld4 r15=[r14];
  sub r32=r15,r35; st4`), cycle 835936154 (step ~50.5M), task `kthreadd`.
  Bytes `00 00 00 c0` = `0xC0000000` (`0 - PREEMPT_ACTIVE`).
- The matching add (`st4 [r2]=r3` at bundle `0x7c050` slot 0, same function)
  executed exactly once (hot-IP 3) but stored to a DIFFERENT task's word:
  IP-filtered logging shows its single store went to PA `0x8ec0c98`
  (`kernel_init` task, value `0x40000000` = correct `0 + PREEMPT_ACTIVE`),
  never to `0x8ec8c98`.
- `preempt_schedule` (entry `0x7c020`) ran exactly once. Its ADD used
  `r13=init` but its SUB used `r13=kthreadd`: `r13` (current) changed across
  the inner `__schedule` without a matching switch-back before the SUB.
  `kernel_init`'s word stays `0x40000000` forever (RAM dump confirms);
  `kthreadd`'s word becomes `0xC0000000`.
- Raw encodings: `0x7c050: 11 00 0c 04 90 11 ... [MIB] st4 [r2]=r3`,
  `0x7c080: 11 00 80 1c 90 11 ... [MIB] st4 [r14]=r32`.
- All later `preempt_count` writes (36 hits: `irq_enter/exit`
  `±HARDIRQ_OFFSET`, `bh_disable/enable` `±SOFTIRQ_OFFSET` at `0x99090`,
  `0x990c0`, `0x98790`, `0x98200`) balance on top of the stuck base.

### 13b. Ticket-lock wedge — stale read in one `fetchadd`

- Full ordered replay of PA `0x8ec7dd8` (52,553 takes + 52,552 unlocks, all
  correct offsets/sizes/values, serving advancing by 1 per unlock): word
  grows `0 → … → 0x9a90cd48` with EXACTLY ONE anomaly at cycle 923620015
  (step 138,193,642): a take (`fetchadd4.acq r3=[r32],1`, bundle `0x829830`
  slot 1, raw `30 18 80 22 21 00 …`) logged new value `0x1003f`
  (i.e. observed old `0x1003e`) while the complete logged history (plus the
  end-state RAM dump, final word `0x1003f`) proves RAM held `0x9a90cd48`.
- Take-entry probe (138.2M budget): `r32=0xe000000008ec7dd8` (correct
  address), `r3=0x1003e` post-take (slot 2 fires after the fetchadd slot).
  Same register feeds the read and the write; the write landed correctly
  (logged at the right PA, RAM confirms), so the READ returned stale/wrong
  data (`0x1003e`, matching five neighboring words) for the correct address.
- Post-anomaly: 1 take, 0 unlocks; the remaining ~112M steps are the slow
  loop alone (18.6M hits). Pre-anomaly: 52,552 takes + 52,552 unlocks,
  zero anomalies.
- Translation ruled out: a second temporary diagnostic logging every
  non-identity DTR/DTLB translation of that VA over 139M steps produced
  ZERO hits (region-7 identity always); the host page table is static
  identity; RSE never writes RAM (`flushrs`/`loadrs` are pointer-only);
  no device covers RAM; no read hooks alter data; MMU hooks see every guest
  store (MMU always enabled), so the logged history is complete.
- Raw encoding of the take bundle:
  `0x829830: 0b 00 00 02 07 00 30 18 80 22 21 00 00 00 04 00`
  (`[MMI]` `rsm`/`fetchadd4.acq r3=[r32],1`/`nop`).

## 14. Root-cause classification

- Progress milestones (§7–8): **A (genuine forward progress)** — no repair.
- `scheduling while atomic` (§12–13a): first incorrect transition proven
  (unbalanced `PREEMPT_ACTIVE` sub across a task change); suspected **C**
  (context-switch/task-resume or `r13` handling), pending Phase 9
  mechanism work. **F (legitimate wait)** for `kernel_init` itself
  (correctly parked in `wait_for_completion`).
- Ticket wedge (§12–13b): first incorrect transition proven (stale
  `fetchadd` read, 1 in ~52K takes on that word); suspected **B/D**
  (read-path staleness), pending Phase 9 mechanism work.
  Not G (budgets suffice: post-anomaly execution is spinner-only with IRQs
  disabled, `pending=1`) and not H (authentic Debian image boots on real
  IA-64 hardware).

## 15. Changes and new tests

- No production-code changes. No new regression tests (nothing repaired).
- Temporary diagnostic (reverted): `GUIDEXOS_WATCH_CORRUPTION` write-watch
  in `tools/ia64_iso_matrix.cpp` (+ `<iomanip>` include) and its
  `build-7r1` binary. Verified reverted via `git status`/`git diff` before
  commit.

## 16. Validation results and unresolved test issues

- `test_new_instructions`: ALL PASS (incl. Phase 7R1 M6/M9 spill/fill,
  Phase 5 RSE/banked-RFI, Phase 6 DEP/cmpxchg/czx1/lfetch).
- `test_ia64_processor_interrupt_block`: PASS.
- `test_ia64_interrupt_controller`: PASS.
- `test_vm_checkpoint`: PASS.
- `git diff --check`: clean. Artifact hashes unchanged (§3).
- Not run per baseline policy: `test_isa_plugin` (>300 s, long EFI
  allocation behavior, previously incomplete — not a pass, not a failure);
  `test_cpu_execution` (pre-existing MinGW `ws2_32` link issue, unrelated).

## 17. Comparison with the Phase 7R1 RCU result

Phase 7R1 crossed the `rcu_sched_qs+0x20` fault into steady RCU/scheduler
cycling with 200 timer interrupts and no death. Phase 8 preserves that
result bit-for-bit (identical counts/IPs/registers) and extends it:
`start_kernel` runs to completion (full dmesg), `rest_init`/`kernel_init`/
`kthreadd` all execute, and the kernel reaches multi-task operation
(`kernel_init` sleeping + `kthreadd` running + idle/RCU/timers) with guest
time advancing past one second.

## 18. Whether Linux attempted or entered userspace

Neither. `init_post` (0 hits), `run_init_process` (0 hits), root mount, and
any userspace execution are all unreached — expected, since `kernel_init`
parks in early initcalls (`do_pre_smp_initcalls`, entry 1 of 6) long before
`do_basic_setup`/`init_post`. **No first-userspace-instruction milestone.**

## 19. Next frontier and recommended Phase 9 scope

1. `preempt_count`: determine how `r13` (or the resumed task) changed across
   the inner `__schedule` in the single `preempt_schedule` invocation
   (cycle ~835.93M): audit `__switch_to`/`context_switch` task selection and
   `r13` save/restore, plus interrupt-entry/exit banked-register handling
   for the surrounding window. Repro: 51M-step IP-filtered watch
   (`0x7c050`/`0x7c080`).
2. Ticket read: determine how the `fetchadd` at cycle 923620015 (step
   138,193,642) returned `0x1003e` for RAM holding `0x9a90cd48` despite clean
   translation, complete write history, and no bypass paths. Candidates:
   host-side read-path staleness (translation-cache coherence,
   store-to-load forwarding if any, `mmuRead` vs `mmuWrite` asymmetry).
   Repro: 139M-step run ( anomaly included); translator logging already
   rules out DTR/DTLB staleness.
3. With either writer mechanism proven, implement the minimal architectural
   repair with a pre-repair failing regression using the authentic raw
   encodings in §13 (`0x7c050`, `0x7c080`, `0x829830` bundles).
4. Re-run the 250M continuation: success = `kthreadd` processes the create
   list (`kernel_thread` ≥ 3 visits), `kernel_init` exits
   `wait_for_completion`, early initcalls complete, and execution reaches
   `smp_init`/`do_basic_setup`/`init_post` with no `scheduling while
   atomic`.
5. Keep the dmesg-from-RAM-dump technique: it is the highest-fidelity
   milestone oracle (guest's own words) and caught both the progression and
   the frontier.

## 20. Commit SHA and remote push status

- Parent: `c82ebbbd70cd359e8ee995532b0dec3f3d8b1565` (branch `IA64`,
  divergence from `origin/IA64` was `0 0` at preflight).
- This commit (single local commit on `IA64`): this report
  (`docs/IA64_POST_MADT_8_BOOT_PROGRESSION.md`) plus the `.phase` advance
  (`7→8` to `8→9`). No production-code, test, artifact, or checkpoint
  changes (temporary diagnostics fully reverted; `git diff --check` clean;
  `test_new_instructions` / interrupt-block / interrupt-controller /
  `test_vm_checkpoint` all pass post-revert).
- Push: not attempted (no `origin2`; GitHub auth unreliable per mission
  constraints; local commits authoritative).

### Guiding principle (preserved)

No guest, checkpoint, EFI, or Linux bytes were modified; no registers forced;
no exceptions suppressed; no init skipped; no device responses fabricated.
The kernel booted itself from the bootloader through `start_kernel` into
multi-task operation — and then told us, in its own log, exactly where it
stopped.
