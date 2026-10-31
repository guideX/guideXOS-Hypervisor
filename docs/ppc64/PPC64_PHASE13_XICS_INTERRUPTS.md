# PPC64 Phase 13 — Bounded pSeries XICS compatibility

**Status:** implementation and hosted acceptance suite complete
**Profile:** POWER9 pSeries, legacy XICS compatibility mode, one vCPU / one interrupt server

Phase 13 adds a VM-owned XICS source controller and interrupt presentation model to the PPC64 platform from Phase 10. A host-configured source can raise the existing guest external interrupt input. A guest handler accepts it with PAPR `H_XIRR`, completes it with `H_EOI`, and returns through `rfid`.

This is a bounded XICS compatibility profile. It does not implement native POWER9 XIVE behavior or claim full pSeries/Linux interrupt support.

## Profile and source namespace

The default platform mode remains `LEGACY_EXTERNAL`. `XICS_COMPAT` explicitly enables XICS and causes the external CPU request to come from the ICP presentation state. The Phase 12 legacy external-injection API rejects requests while XICS owns that input; DEC remains a separate CPU interrupt.

The host-owned source subset is `0x1100` through `0x113f` (64 IDs), server 0 only. QEMU defines the sPAPR XICS base at `0x1000` and the VIO device range beginning at base plus `0x100`; this subset stays inside that VIO range. Linux reserves XICS source numbers below 16, including zero for no interrupt and two for IPI. The subset therefore avoids those low reserved IDs. Each source ID is unique within one VM, while separate VMs can use the same ID independently. [QEMU sPAPR IRQ definitions](https://github.com/qemu/qemu/blob/master/include/hw/ppc/spapr_irq.h), [Linux XICS source reservations](https://codebrowser.dev/linux/linux/arch/powerpc/kvm/book3s_xics.h.html)

The host API configures LEVEL or EDGE sources, priority, destination server, owner LPID, and enable state. Enabled sources cannot use priority `0xff`. Only root LPID 0 or a live partition in the same VM may own a source. There is no global IRQ registry or guest source-configuration hypercall.

## ICS and ICP behavior

The ICS is a fixed 64-entry table. Each entry records allocation, type, enable state, server, priority, assertion, pending, presentation, acknowledgment, and owner LPID. An EDGE event latches until accepted; a new rising edge while an earlier edge is acknowledged can remain pending as the next event. A LEVEL source remains pending while asserted and is presented again after EOI if it is still enabled and asserted. Disabling a pending source masks its presentation without discarding its pending state.

The ICP keeps CPPR, MFRR, selected source and priority, plus a bounded stack of outstanding XIRR values for each LPID. Lower numeric priority is stronger; a source is eligible only when `priority < CPPR`. The selected eligible source has the lowest numeric priority. Equal priorities are resolved by the lowest source ID. A new source does not overwrite an accepted source: source pending state is per entry, while outstanding acknowledgments are bounded to the 64-source limit.

On XIRR acceptance, the returned XIRR is the current packed presentation value, with CPPR in bits 31:24 and XISR in bits 23:0. Acceptance clears the selected presentation and sets CPPR to that source's priority. `H_EOI` must supply the exact most recently accepted XIRR for that active LPID; it restores the saved CPPR and completes only the matching source. Invalid, stale, cross-LPID, out-of-range, or out-of-order EOI values return `H_PARAMETER`. With no source selected, XIRR returns the current CPPR with XISR zero and advances CPPR to `0xff`. The field layout and accept/EOI flow follow QEMU's XICS ICP behavior. [QEMU XICS ICP implementation](https://github.com/qemu/qemu/blob/master/hw/intc/xics.c)

## PAPR hypercalls

The existing production `sc 1` dispatcher implements these operations:

| Hypercall | Input | Output | Behavior |
|---|---|---|---|
| `H_EOI` `0x64` | `r4`: exact 32-bit XIRR | `r3`: status | Completes the matching acknowledged source and restores prior CPPR. |
| `H_CPPR` `0x68` | `r4`: priority byte | `r3`: status | Sets active ICP CPPR and reevaluates pending sources. |
| `H_IPOLL` `0x70` | `r4`: server 0 | `r3`: status, `r4`: XIRR, `r5`: MFRR | Reads presentation state without accepting an interrupt. |
| `H_XIRR` `0x74` | none | `r3`: status, `r4`: XIRR | Accepts the currently presented source. |
| `H_XIRR_X` `0x2fc` | none | `r3`: status, `r4`: XIRR, `r5`: deterministic virtual time base | Extended XIRR acceptance; uses the configured virtual clock, never host wall time. |

Successful operations return `H_SUCCESS` in `r3`; malformed arguments return `H_PARAMETER`. Calls retain the existing privilege checks. `H_IPI` `0x6c` is explicitly unsupported for this single-server profile: a syntactically valid self-target request returns `H_FUNCTION`; invalid server or priority fields return `H_PARAMETER`. Multi-vCPU IPI delivery is not implied. The opcode values match Linux's PAPR hypercall definitions. [Linux PowerPC `hvcall.h`](https://codebrowser.dev/linux/linux/arch/powerpc/include/asm/hvcall.h.html)

## CPU delivery and DEC

An eligible active-LPID ICP presentation asserts the plugin's existing external request. The VM checks the ordinary PPC64 interrupt boundary and MSR[EE], enters vector `0x500`, saves the already-established NIP in SRR0, and does not count the boundary event as an executed guest instruction or virtual-time tick. The handler runs normal encoded PPC64 instructions and returns through `rfid`.

DEC remains a CPU architectural interrupt at vector `0x900`; it is never an ICS source and is not cleared by XIRR or EOI. When DEC and XICS are eligible together, external delivery keeps Phase 12 precedence. DEC remains pending through XICS acknowledgment/EOI and is delivered after the guest return when eligible.

## Device-tree discovery

XICS mode adds one root child at `/interrupt-controller` with:

| Property | Value |
|---|---|
| `device_type` | `PowerPC-External-Interrupt-Presentation` |
| `compatible` | `IBM,ppc-xicp` |
| `interrupt-controller` | empty property |
| `#interrupt-cells` | `<2>` |
| `ibm,interrupt-server-ranges` | `<0 1>` (server 0 only) |
| `phandle`, `linux,phandle` | `<1>` |

The independently parsed DTB test checks property names, bytes, cell widths, node path, phandles, and the one-server range. It also checks the node is absent when XICS is disabled. The tree does not add XIVE properties such as `ibm,power-ivpe`. These bindings match QEMU's sPAPR XICS node construction. [QEMU sPAPR XICS device-tree construction](https://github.com/qemu/qemu/blob/master/hw/intc/xics_spapr.c)

## Ownership, lifecycle, and checkpoints

Each `VirtualMachine` owns its own XICS object. Presentation state is keyed by LPID; active partition changes bind the CPU to that LPID's ICP. Host source configuration verifies that nonzero owner LPIDs exist in that VM. Guest acknowledgment operates only on the active LPID. Destroying a VM releases its controller with no callbacks into another VM.

Reset clears all source allocations and presentation records, returns to root LPID 0, CPPR 0, and MFRR `0xff`. Pause and snapshot capture preserve pending, presented, acknowledged, and asserted conditions. Restore validates the XICS version, source IDs and owners, source/presentation relationships, arbitration result, outstanding XIRR records, active LPID, and platform mode before committing staged VM state.

The PPC64 checkpoint writer now emits version 5 / payload 5 and stores the platform interrupt mode and bounded XICS state. Readers continue to accept v1 through v4 with the older payload layouts; those formats resolve to legacy interrupt mode with no XICS state. The Phase 12 DEC and virtual-time-base checkpoint data remains serialized in payload v4 and later. Corrupt counts, duplicate sources, invalid priorities or servers, inconsistent presentations, cross-architecture input, incompatible platform modes, and checksum corruption are rejected.

## Programs A–K

`tests/test_ppc64_phase13.cpp` drives the production `VirtualMachine` with genuine PPC64 instructions and `sc 1` calls. Host calls are used only to configure and assert sources or inspect controller state.

| Program | Result |
|---|---|
| A — initialization | Guest CPPR and IPOLL; first allocated source; XICS capability and packed XIRR. |
| B — first interrupt | External vector `0x500`, SRR0/SRR1/MSR checks, guest H_XIRR/H_EOI, memory record, `rfid`. |
| C — arbitration | Different and equal priorities, deterministic tie, and a higher-priority arrival while an earlier XIRR remains outstanding. |
| D — masking | CPPR-masked source, IPOLL without acknowledgment, later CPPR eligibility, and wrong-server rejection. |
| E — level source | EOI reasserts an asserted LEVEL source; deassertion ends delivery. |
| F — DEC interaction | External XICS event is delivered before DEC; each is handled at its own vector. |
| G — DTB and XIRR_X | Independent FDT parse and XIRR_X result/time-base advancement. |
| H — VM/LPID isolation | Two VMs use the same source ID under separate LPID 7 contexts; root H_XIRR cannot acknowledge the partition source; one VM is destroyed while the other remains valid. |
| I — checkpoint | No-pending, masked-pending, presented-unacknowledged, acknowledged, and still-asserted LEVEL states; VM/file restore; invalid snapshot and checksum rejection. |
| J — terminal | Guest handler accepts a source, writes guest memory, emits `XICS` through PAPR virtual terminal, EOI, and returns. |
| K — resilience | Unsupported IPI, bad IDs/servers/priorities/EOI, source cleanup constraints, disable/re-enable, reset, and bounded inaccessible-handler failure. |

**Phase 13 result: 539 assertions, 0 failures.**

## Regression evidence

All prior PPC64 phase test targets passed in the final CTest run. The Phase 1–12 counts below are the established suite counts; Phase 13 adds 539 assertions.

| Phase | Existing checks | Result |
|---|---:|---|
| PPC64-1 | 148 | Pass |
| PPC64-2 | 399 | Pass |
| PPC64-3 | 535 | Pass |
| PPC64-4 | 378 | Pass |
| PPC64-5 | 496 | Pass |
| PPC64-6 | 183 | Pass |
| PPC64-7 | 194 | Pass |
| PPC64-8 | 721 | Pass |
| PPC64-9 | 231 | Pass |
| PPC64-10 | 232 | Pass |
| PPC64-11 | 974 | Pass |
| PPC64-12 | 563 | Pass |
| **Prior PPC64 total** | **5,054** | **Pass** |
| PPC64-13 | 539 | Pass |

Final CTest registered 25 cases: all 19 available executables passed, including Basic/CPU execution, new instructions, EFI memory map, IA-64 SAL, VM checkpoint, RISC-V decoder smoke, and PPC64 Phases 1–13. Six pre-existing entries remained **Not Run** because their executables were absent: syscall dispatcher, debugger, profiler, fuzzer, VM isolation, and ELF validation. The prior baseline had the same six unavailable targets (18/24 passed before Phase 13 was registered). No shared or IA-64 test that was available in the baseline regressed.

Standalone shared/IA-64 checks also passed: `test_ia64_acpi` (7 cases), `test_ia64_interrupt_controller`, and `test_ia64_processor_interrupt_block`. The standalone `test_isa_plugin` executable was built and run; it exits 1 at the known IA-64 NaT serialization assertion, `tests/test_isa_plugin.cpp:1551` (`GetGRNaTPhysical(NUM_GENERAL_REGISTERS)`). This is the legacy IISA payload limitation recorded in earlier phase documentation; Phase 13 does not change IA-64 state serialization.

The MinGW WinSock linker problem is a separate pre-existing build limitation. An explicit `test_syscall_dispatcher` build failed at link time on unresolved `__imp_WSAStartup`, `__imp_socket`, `__imp_bind`, `__imp_listen`, `__imp_setsockopt`, `__imp_WSAGetLastError`, `__imp_closesocket`, `__imp_WSACleanup`, `__imp_accept`, `__imp_recv`, and `__imp_send` imports from `src/webui/WebUIServer.cpp`. The other five absent CTest targets were not individually diagnosed in this run; their combined build attempt stopped in Ninja. None of these targets is part of the PPC64/XICS acceptance path.

The historical MMU Hook-based Monitoring failure is unchanged and was not rerun in Phase 13. Phase 8 records both the saved baseline and standalone binaries at 10/11 with the same `PERMISSION_WRITE` diagnostic. Phase 13 changes no `memory.cpp`, `mmu.cpp`, or MMU test code, and this result is not claimed as fixed or newly introduced.

## Limits and follow-on work

This is not native XIVE/TIMA support. It has no XIVE event queues, CAS mode negotiation, SMP, multi-server routing, guest source allocation, RTAS interrupt configuration, PCI/MSI integration, or full RTAS implementation. Host-preconfigured sources are suitable for future virtual-device work, but unmodified Linux may need firmware/RTAS services that are not present here. Do not infer Linux boot support from the XICS hypercalls and DT node alone.

Future device integration should add bounded source ownership to each device, assert/deassert through the VM API, preserve source level/edge semantics during device reset, and include the pending device and ICP state in checkpoint validation. PPC64-14 should focus on connecting one emulated VIO device to the host-owned source API and testing its reset/checkpoint path without expanding XIVE or SMP scope.
