# IA64-POST-MADT-9R5 — Single-CPU Guest Context Switching

## 1. Outcome

**Outcome A — single-CPU context switching verified.** Guest instructions switched A to B to A to B to A on one persistent virtual IA-64 CPU. The original main-frame values, return addresses, PFS/CFM, backing-store ownership, and the tested NaT state survived the repeated switches. The required existing regression executables passed.

Automatic RSE pressure remains incomplete. A bounded legal-frame pressure probe completed but reproduced missing automatic spill/fill behavior; this is recorded as an open R6 item and is not represented as complete RSE support.

The `.phase` gate remains at `IA64-POST-MADT-8` → `IA64-POST-MADT-9` as required.

## 2. Git and workspace state

- Starting branch: `IA64`
- Starting HEAD: `903930f792dcccdeb933b1e64e09359933f95d96`
- Starting phase: `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`
- Ending branch: `IA64`
- Ending commit: recorded in the closeout below after the local commit
- Push status: not pushed
- `.phase`: unchanged
- No branch/worktree changes, merges, resets, or remote operations were made.
- Existing untracked build and scratch directories were preserved. `build-9r5/` was added for this validation and is also left in place.

## 3. One-CPU test architecture

`tests/test_ia64_single_cpu_context_switch.cpp` builds bundles in guest RAM and runs them through `VirtualMachine::stepCPU(0)`, the registered production `IA64ISAPlugin`, production instruction decoding, and production instruction execution. The main scenario uses one VM, one guest address space, one `CPU` object, and one `CPUState` object. The test retains and checks the CPUState address on each step. It never swaps a CPUState, loads a host snapshot, or calls an emulator task-switch API.

The synthetic guest has shared code at `0x12000` and `0x13000`; A and B have guest context records at `0x6000` and `0x6100`, separate backing stores at `0x8000` and `0x9000`, and verification regions at `0x7000` and `0x7200`. The fixture starts A in the one live CPU and seeds B's ready-to-run register image and record in guest RAM. From the first guest call onward, the guest performs the switching procedure.

## 4. Guest instruction sequence and save layout

The guest's shared nested routine executes `alloc`, copies context-owned marker values into its local frame, and uses `br.call b1` to enter the switch procedure. The switch procedure:

1. Executes `alloc`, retaining the caller PFS in its local frame.
2. Saves outgoing RSC, BSP, and caller PFS to the outgoing guest record.
3. Executes `flushrs` in its own instruction group.
4. Clears the RSC mode bits to select enforced-lazy mode, then saves the post-flush RNAT and BSPSTORE.
5. Saves `b0`, `b1`, GP, SP, TP, the guest verification cursor, and two static markers.
6. Executes `invala` before changing contexts.
7. Loads the incoming record with guest `ld8` instructions, switches BSPSTORE while RSC is lazy, restores BSP, PFS, RNAT, and the incoming RSC, then restores branches and static state.
8. Uses guest register moves to exchange the outgoing and incoming context-record cursors and executes `br.ret b1` into the incoming context.

This sequence follows the synchronous backing-store switch procedure in Intel's *Itanium Architecture Software Developer's Manual*, Volume 2, §6.11.3 ([manual PDF](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf)). The guest executes `invala` before installing the incoming context, providing ALAT invalidation for this switch.

The generated memory instructions use architecturally decoded M5 post-increment forms and instruction groups with stop bits. The synthetic context record is:

| Offset | Guest-owned value |
|---:|---|
| `+0` | RSC |
| `+8` | BSP |
| `+16` | caller PFS/CFM |
| `+24` | RNAT |
| `+32` | BSPSTORE after flush |
| `+40`, `+48` | `b0`, `b1` |
| `+56`, `+64`, `+72` | GP, SP, TP |
| `+80` | verification cursor |
| `+88`, `+96` | context-owned static markers |

Each context starts with SOF=6 and SOL=4. The test uses identical shared call and return instruction addresses in A and B. In the repeated run, the first switch enters B at its saved `kBMain` entry; the remaining three returns resume at the identical shared target `0x12060`. At suspension, the main-to-nested `b0` return and nested-to-switcher `b1` return are both outstanding; both are saved and restored by the guest procedure.

## 5. First architectural mismatch and repair

The first failing transition after correcting the synthetic RSC setup was A's `br.ret` from the shared nested routine to A's main frame (`0x12080`). The expected return fill was to restore A's stacked values and preserve the live NaT collection for the same 512-byte RNAT region. The old implementation instead read the collection slot at `0x81f8`, which `flushrs` had not materialized because the registers and collection were still represented by the live AR.RNAT in that region. The unwritten zero replaced the live collection, clearing GR35's NaT bit. The failed invariant was: **the restored A frame must retain its original GR35 value and NaT state**.

The focused repair is in `restoreRseReturnFrame` in `src/decoder/decoder.cpp`: a return fill now uses live AR.RNAT when the fill address belongs to the live BSPSTORE RNAT region, and reads a backing-store collection for other regions. The same rule is applied when selecting the post-fill RNAT value. It changes no CPUState layout and introduces no context ownership map.

The causal chain was:

> Expected: fill A's frame and retain live RNAT bit 3 → Observed: read zero from an unwritten backing collection slot → Failed invariant: A's GR35 NaT state was cleared.

## 6. Switching and state results

- A → B → A: passed as the first prefix of the same guest run that continued through repeated alternation.
- A → B → A → B → A: passed; the trace recorded 864 instructions and four switcher returns.
- One CPUState pointer was retained and checked before each production VM step.
- Both contexts returned through the identical shared target `0x12060`; A's outer return continued to its original main code.
- A's distinguished stacked values and GP/SP/TP were checked after both resumes. B's values and GP/SP/TP were checked after B resumed once, and its GP/SP/TP plus still-suspended second-call cursor were checked at the next suspend.
- The outgoing records retained each context's RSC, PFS/CFM, BSP/BSPSTORE boundary, RNAT bit, both branch return addresses, GP/SP/TP, and static markers. Assertions check each saved outer main return address as well as the shared nested return address.
- B's backing-store words remained unchanged by A's flush/switch. A's NaT-marked stacked GR35 returned with its NaT bit set.
- B's NaT-marked stacked GR35 was observed with its NaT bit set after the shared nested return.
- The test verifies two outstanding calls at suspension (main → nested and nested → switcher) for each context and resumes both branch return addresses. It does not model a general-purpose scheduler or arbitrary-depth continuation graph.

## 7. Automatic RSE spill/fill pressure

A separate bounded guest creates three legal 64-register frames (192 live stacked-register positions) against a 96-register window, using `alloc`, `br.call`, and `br.ret`. It watches BSPSTORE and a sentinel-filled guest backing-store region, then checks two distinguished caller registers after return.

Observed result: the guest reached its continuation in 150 instructions, but BSP and BSPSTORE both remained `0xa000`, BSPSTORE never advanced, all seeded backing words were unchanged, and the two caller values were not restored. The probe reports this as a reproduced gap rather than a passing spill/fill test. It isolates the missing behavior to automatic RSE backing-store transfer during frame pressure; because no automatic spill occurred, a corresponding post-spill fill could not be qualified. Explicit `flushrs`/`loadrs` and the new live-RNAT return-fill regression remain separate behavior.

No broad automatic RSE rewrite was attempted. The next bounded task should implement automatic spill/fill using the existing backing-store transfer machinery, then turn this probe into a passing pressure regression that checks value and NaT bits, memory faults/translation, BSP/BSPSTORE, and collection boundaries.

## 8. Checkpoint implications

The repair changes no serialized CPUState/RSE fields, so checkpoint format/version handling was not changed. `test_vm_checkpoint` passed. The accepted v5 Linux checkpoint was not modified or migrated. Full suspended-call-chain checkpoint continuation remains deferred.

## 9. Validation

Configured and built with CMake/Ninja Release in `build-9r5/`. The new target and required existing tests were executed:

| Executable | Result |
|---|---|
| `test_ia64_single_cpu_context_switch` | Passed A → B → A → B → A; emitted the bounded automatic-RSE gap described above |
| `test_new_instructions` | Passed |
| `test_ia64_processor_interrupt_block` | Passed |
| `test_ia64_interrupt_controller` | Passed |
| `test_vm_checkpoint` | Passed |
| Focused `br.ret` live-RNAT return-fill regression | Passed inside the new target |
| `git diff --check` | Passed |

No ISO build, Linux boot, or QEMU run was required. `test_isa_plugin` was not run; its known runtime and MinGW Winsock issues remain outside this R5 qualification.

## 10. Remaining gaps and R6 scope

R5 proves guest-driven same-CPU context switching for the tested resident frames, including two outstanding call returns, identical return sites, separate context records/backing stores, and repeated alternation. It does not establish automatic RSE spill/fill under register-window pressure, arbitrary-depth stacked-frame handling, or suspended-call-chain checkpoint continuation. R6 should first implement and qualify bounded automatic spill/fill, then extend context-switch coverage to deeper pressure-driven frames. Authentic Linux scheduler qualification remains later work.

## 11. Closeout

- Ending commit: to be filled after local commit.
- Ending branch: `IA64`.
- Worktree: tracked changes committed; pre-existing untracked build/scratch directories preserved, with `build-9r5/` retained as the R5 build and logs.
- Push status: not pushed.
