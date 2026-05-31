# IA-64 Feature Gap Analysis

Date: 2026-05-11

Scope: guideXOS Hypervisor source tree audit for Itanium / IA-64 platform readiness. This document is based only on source files and logs visible in this repository. It does not claim any feature works unless code or logs show evidence.

Primary source areas reviewed:

- `include/decoder.h`, `include/ia64_opcodes.h`
- `src/decoder/decoder.cpp`
- `src/decoder/atype_decoder.cpp`, `itype_decoder.cpp`, `mtype_decoder.cpp`, `btype_decoder.cpp`, `ftype_decoder.cpp`, `xtype_decoder.cpp`, `lx_decoder.cpp`
- `include/cpu_state.h`, `include/cpu.h`, `src/core/cpu.cpp`, `src/core/cpu_core.cpp`
- `include/IA64ISAPlugin.h`, `src/isa/IA64ISAPlugin.cpp`
- `include/memory.h`, `src/memory/memory.cpp`, `include/mmu.h`, `src/mmu/mmu.cpp`
- `src/vm/VirtualMachine.cpp`, `src/vm/VMManager.cpp`
- `include/PEParser.h`, `src/storage/PEParser.cpp`
- `src/devices/*`, `src/storage/*`
- `tests/test_new_instructions.cpp`, `tests/test_isa_plugin.cpp`, `tests/test_mmu.cpp`
- `output.log`
- `guideXOS Hypervisor GUI/bin/Debug/net9.0-windows/logs/native-20260510-220921.log`
- `guideXOS Hypervisor GUI/bin/Debug/net9.0-windows/logs/native-error-20260510-220921.log`

## Status Legend

- Implemented: code provides an actual implementation and tests/logs give at least local evidence.
- Partially implemented: code handles a useful subset or has pragmatic boot-specific behavior, but not the full architectural feature.
- Stubbed: an interface or instruction path exists but returns success/unsupported, no-ops, skips, or synthetic values.
- Missing: no meaningful implementation was found.
- Unknown / needs test: code may have support, but no strong source/log/test evidence proves behavior.

Priorities:

- P0: likely blocks current ISO boot or EFI loader progress.
- P1: likely blocks Linux kernel handoff or early kernel execution.
- P2: important IA-64 correctness or compatibility, but probably after handoff.
- P3: long-term completeness, performance, or fidelity.

## Executive Readout

The current tree has a meaningful IA-64 skeleton: bundle extraction, slot routing, selected integer/memory/branch instruction decoding, a CPU state object with IA-64-shaped register files, a simplified flat memory/MMU layer, PE/COFF loading, synthetic EFI table construction, and a growing IA-64 ISA plugin with EFI stub intercepts.

The implementation is not yet a real IA-64 platform. The largest gaps are not isolated single instructions. They are platform contracts: correct EFI file and boot services, GP/function descriptor semantics, IA-64 relocation coverage, register stack engine behavior, interruption state delivery, virtual memory/region/TLB support, and a real Linux-compatible firmware/device model.

The latest native log confirms the EFI application starts and reaches several EFI stubs. It does not confirm kernel handoff. The strongest current evidence is:

- PE/COFF image parsing and IA-64 entry descriptor detection occur.
- EFI handoff registers `r32` and `r33` are set.
- The bootloader invokes `HandleProtocol`, `GetVariable`, `OutputString`, and `AllocatePool` stubs.
- Console output is mirrored to VM console/framebuffer.
- The EFI app returns through top-level `br.ret b0` before kernel handoff.
- `SimpleFS.OpenVolume calls=0` in the final milestone.
- The error log shows out-of-bounds load/store recovery near loader code, including wild addresses that look like corrupted UTF-16/pointer data.

## 1. IA-64 Instruction Decoding

### Summary

| Feature | Status | Evidence | Risk, symptoms, block, priority |
|---|---|---|---|
| Bundle extraction | Partially implemented | `InstructionDecoder::DecodeBundleAt`, `ExtractTemplate`, `ExtractSlot` in `src/decoder/decoder.cpp`; `Bundle` in `include/decoder.h` | Matters because all IA-64 execution starts from 16-byte bundles. The implementation extracts 3 41-bit slots and template bits, but assumes little-endian host layout and simplified execution. Bad extraction causes random opcodes or wrong control flow. Blocks ISO boot now if incorrect. P0. |
| Template decoding | Partially implemented | `GetUnitsForTemplate` table in `src/decoder/decoder.cpp`; `TemplateInfo` and `Template` enum in `include/ia64_opcodes.h` | Matters because slot unit type and stop bits depend on the template. Reserved templates are present as dummy mappings. Stop bits are represented but not used for full EPIC dependency semantics. Symptoms include incorrect instruction grouping, predicate timing errors, or invalid slot decoder selection. Blocks ISO boot now for loader bundles; Linux later for wider code. P0/P1. |
| Slot decoding | Partially implemented | `InstructionDecoder::DecodeSlot` dispatches to M/I/B/F/L/X decoders | Decoders exist for all major slot families, but each covers a subset. Unknowns are frequent in older `output.log`. Missing slot forms can become no-ops or exceptions. Blocks ISO boot now when loader reaches uncovered forms. P0. |
| Stop-bit semantics | Stubbed | `Bundle::stopAfterSlot`, `Bundle::hasStop`; plugin predicate snapshots at bundle/stop boundaries | Matters for IA-64 instruction group semantics and predicate writes. Current execution is mostly sequential, with a targeted predicate snapshot mechanism. Symptoms include branches or predicated instructions observing same-group results too early/late. Linux later and correctness gap. P1/P2. |
| Predicate handling | Partially implemented | `InstructionEx::Execute` checks predicates; plugin has `predicateGroupSnapshot_`; tests cover predicate groups | False predicates nullify most instructions. The plugin snapshots predicate groups for non-branch instructions but branches use live predicates. This may match some tested boot cases but not full IA-64 group semantics. Symptoms are wrong conditional execution and wrong branch decisions. Blocks loader/kernel as soon as dependent predicates matter. P1. |
| A-type integer ALU | Partially implemented | `ATypeDecoder::decode`, `InstructionType` cases in `InstructionEx::Execute`; tests in `tests/test_new_instructions.cpp` | Handles useful subset: add/sub/addp4, and/or/xor/andcm, shladd, compare families. Full IA-64 ALU completers and forms are not present. Symptoms include skipped arithmetic, bad address generation, and wrong comparisons. Blocks ISO boot now for uncovered loader code; Linux later. P0/P1. |
| Compare completers | Partially implemented | `CompareType` and compare execution in `InstructionEx::Execute`; tests for compare forms | Several compare completers are modeled, but not the complete predicate-producing IA-64 compare matrix. Bad predicates cascade into wrong control flow. Blocks both loader and Linux. P1. |
| I-type integer/special | Partially implemented | `ITypeDecoder::decode`; execution for shifts, dep/extr, zxt/sxt, alloc, tbit/tnat, PR/AR/BR moves | Several key boot instructions exist, including `ALLOC`, `MOV_TO_AR`, `MOV_FROM_AR`, `MOV_TO_BR`, `MOV_FROM_BR`, `MOV_TO_PR`, and `MOV_FROM_PR`. Many I-unit forms are not implemented. Symptoms include unsupported instruction exceptions or silent no-ops depending on execution path. Blocks ISO boot now when hit. P0. |
| Branch instructions | Partially implemented | `BTypeDecoder::decode`; `BR_COND`, `BR_CALL`, `BR_RET`, `BR_CLOOP` execution in `InstructionEx::Execute`; plugin call/return handling | Basic cond/call/ret/cloop exist. `BR_IA`, `BR_CTOP`, `BR_CEXIT`, `BR_WTOP`, `BR_WEXIT` are decoded but have no execution cases. Decoder includes boot-specific heuristics for raw return/call patterns. Symptoms include broken loops, indirect branches, or function calls. Blocks ISO boot now and Linux later. P0/P1. |
| Call/return/function descriptors | Partially implemented | Plugin `handleIndirectCall` style behavior in `src/isa/IA64ISAPlugin.cpp`; PE parser entry descriptor detection; EFI function descriptor tables in `VMManager.cpp` | Function descriptors are recognized in PE entry and synthetic EFI service tables. Some call bridging and top-level return handling exist. This is still fragile and boot-specific. Symptoms include wrong `ip`, wrong `gp`, returns to zero, or loader returning before kernel handoff. Blocks ISO boot now. P0. |
| GP-relative addressing | Partially implemented / risky | PE parser records `globalPointer`; VM setup writes CPU `r1`; native log warns entry descriptor GP outside `SizeOfImage` and mirrors non-exec sections | IA-64 code heavily relies on `gp`. Current log says `GP=0x238000 is outside SizeOfImage=0x5e000`, with a diagnostic mirror workaround. Missing relocation/data-region correctness causes corrupt pointers and out-of-bounds memory references. Blocks ISO boot now. P0. |
| Floating point decode | Partially implemented | `FTypeDecoder::decode` recognizes FMA/FMS/FNMA/XMA/FCMP/FCLASS/FRCPA/FRSQRTA/FMIN/FMAX/FMERGE/FCVT families | Decode coverage exists for several families, but execution is mostly missing. Symptoms are silent no-op math or exceptions if later paths reject unknowns. Linux later; completeness gap unless loader uses FP. P2. |
| Floating point execution | Stubbed / missing | `InstructionEx::Execute` only has limited `GETF_SIG` and `FCVT_FX`/`FCVT_FXU` behavior | Matters for Linux user/kernel FP state, EFI code that uses FP, and architectural fidelity. Symptoms are incorrect FP values and broken context state. Mostly Linux later and long-term completeness. P2/P3. |
| SIMD/media instructions | Missing | No meaningful media/SIMD execution path found; some names/comments exist | IA-64 includes media/SIMD-like instruction groups. Missing support will break optimized firmware/kernel/library code that uses them. Likely Linux later or completeness. P2/P3. |
| Privileged/system instructions | Stubbed / missing | Generic AR/BR/PR moves exist; `CPU::servicePendingInterrupt` and plugin interrupt stubs; no PAL/SAL/SAPIC/control register semantics found | Linux IA-64 depends on privileged state, PSR, interruption registers, region registers, TLB controls, and platform calls. Missing behavior leads to early kernel faults or hangs. Blocks Linux later. P1. |
| Break instruction | Stubbed | `BREAK` decoded by X-type; execution only dispatches syscall for immediate `0x100000` if a dispatcher exists | IA-64 break is a real interruption class. Current generic break handling is not full fault delivery. Symptoms: missed firmware/kernel break services, no debug/trap behavior. Linux later and diagnostics. P1/P2. |
| NOP/MOVL X/LX | Partially implemented | `XTypeDecoder`, `LXDecoder::combineMOVL`, plugin boot MOVL tests | MOVL reconstruction exists in `LXDecoder`, but `formats::reconstructImm64()` in `include/decoder.h` is stubbed to return 0. If any path uses the stub helper, immediates corrupt. Blocks ISO boot if affected. P0/P1. |
| Advanced/speculative loads | Stubbed | M-type decodes `LD_S`, `LD_A`, `LD_SA`; `CHK_A` execution returns success | IA-64 speculative/advanced loads depend on NaT and ALAT behavior. Treating them as normal loads or success stubs can hide faults and create bad data. Linux later; bootloader risk if used. P1/P2. |
| Unimplemented instruction fallback | Partially implemented / risky | Legacy CPU logs and skips unknowns in `src/core/cpu_core.cpp`; plugin logs unsupported and returns `ExecutionResult::EXCEPTION`; plugin may recover failed loads by zeroing registers | Divergent fallback behavior makes debugging hard. Skipping unknowns can falsely progress; hard exceptions can halt. Recovery can hide real bugs. Symptoms include silent wrong execution, loader returning early, or confusing logs. Blocks ISO boot now and Linux later. P0. |

### Decoder-specific observations

- `include/decoder.h` explicitly labels the instruction model as simplified and notes TODO work for full EPIC parallel execution.
- `InstructionEx::Execute` is a central execution path for many decoded instructions, but plugin code also adds boot-specific behavior and recovery around it.
- `tests/test_new_instructions.cpp` and `tests/test_isa_plugin.cpp` provide valuable narrow coverage for recent boot blockers, predicate groups, loops, raw branch encodings, MOVL, memory operations, and EFI stubs. They do not prove broad IA-64 ISA coverage.

## 2. CPU Architectural State

### Register and State Coverage

| Feature | Status | Evidence | Risk, symptoms, block, priority |
|---|---|---|---|
| General registers | Partially implemented | `CPUState` stores 128 GRs and NaT bits; `GR0` forced to 0 in `SetGR` | Basic storage exists. Register rotation/RSE is incomplete and many execution paths access raw registers directly. Symptoms include wrong local/output registers after calls. Blocks Linux later and can affect EFI now. P1. |
| Floating registers | Stubbed / partially implemented | `CPUState` stores 128 16-byte FP registers | Storage exists, but architectural initialization and FP operations are mostly absent. IA-64 `f0`/`f1` architectural constants are not clearly initialized. Symptoms include wrong FP/signature movement. Mostly Linux later. P2. |
| Predicate registers | Partially implemented | `CPUState` stores 64 predicates; `PR0` forced true; plugin snapshot handling | Basic predicate file exists. Group timing and rotating predicates are incomplete. Symptoms include incorrect predication and loop behavior. Blocks Linux later, may block EFI paths. P1. |
| Branch registers | Partially implemented | `CPUState` stores 8 BRs; branch execution reads/writes BRs | Storage and basic call/return usage exist. Function descriptor and indirect branch handling are boot-specific and incomplete. Symptoms include wrong return/call targets. Blocks ISO boot now. P0. |
| Application registers | Partially implemented | `CPUState` stores 128 ARs; I-type moves to/from AR; `AR64`/PFS and `AR65`/LC receive special cases | Generic storage exists. Only selected AR semantics are modeled. RSC, BSP, BSPSTORE, RNAT, UNAT, FPSR, CCV, EC are not meaningfully implemented. Blocks Linux later and RSE correctness. P1. |
| Control registers | Missing / unknown | No distinct IA-64 control register file found; generic AR scratch used for interrupt info | Linux expects control registers for interruption, translation, and privileged state. Missing support prevents real fault delivery and virtual memory. Blocks Linux later. P1. |
| PSR | Stubbed / risky | `CPUState::psr_`; `CPU::setInterruptsEnabled` uses bit 0; plugin uses bit 14 for PSR.i | PSR is central to IA-64 privilege, interrupts, banked regs, endianness, and execution mode. Current bit inconsistency is a correctness risk. Symptoms: interrupts enabled/disabled incorrectly. Linux later. P1. |
| IPSR/IIP/IFS | Missing / stubbed | `CPU::servicePendingInterrupt` stores vector in AR0 and old IP in AR1; plugin comment says real IIP/IPSR not implemented | IA-64 interruptions require preserved IIP, IPSR, IFS, IIM, ISR, IFA, ITIR, etc. Without them Linux cannot take faults or interrupts correctly. Blocks Linux later. P1. |
| NaT bits | Partially implemented | Per-GR NaT array; `SetGR` clears NaT; `TNAT`/`SHRP` handling | Storage exists, but NaT propagation, speculative load behavior, RNAT/UNAT backing store, and NaT consumption faults are missing. Symptoms: hidden bad pointers or missing faults. Linux later and speculative-load correctness. P1/P2. |
| Register stack engine | Stubbed | `ALLOC` updates CFM fields; plugin `callFrameStack_` and pending call input bridge; no backing store engine | IA-64 call ABI depends on RSE, backing store, CFM/PFS, dirty partition, and spills/fills. Current implementation is synthetic. Symptoms: arguments/locals lost across calls, wrong returns, stack corruption. Blocks Linux handoff and can affect bootloader. P0/P1. |
| Backing store | Missing | No real BSP/BSPSTORE/RNAT spill/fill engine found | Required for deep calls and OS context switching. Symptoms include missing stacked registers after overflow or kernel RSE setup failure. Blocks Linux later. P1. |
| Current frame marker | Partially implemented | `CPUState::cfm_`; `ALLOC` saves old CFM to destination and writes SOF/SOL/SOR | CFM fields exist but are not tied to full register frame allocation/backing store. Symptoms include wrong logical register windows. Blocks Linux later; possible EFI call issues. P1. |
| Rotating registers | Partially implemented / risky | `CPU::mapLogicalToPhysicalReg` handles GR/FR/PR rotation based on CFM RRB fields | Mapping exists in `CPU`, but many instructions execute through direct `CPUState` access in `InstructionEx::Execute`, bypassing rotation. Symptoms include wrong software-pipelined loops. Linux later. P2. |

### Architectural-state notes

- `CPUState` is a useful container, not a complete IA-64 processor state model.
- The plugin adds pragmatic call-frame handling to move boot forward. It should not be treated as architectural RSE.
- Interrupt state currently uses synthetic AR writes and IP jumps rather than IA-64 interruption registers and vectors.

## 3. Memory Model

| Feature | Status | Evidence | Risk, symptoms, block, priority |
|---|---|---|---|
| Flat physical memory | Implemented | `Memory` vector storage in `src/memory/memory.cpp`; bounds checks; device ranges | Basic physical memory works and is tested. Out-of-bounds accesses are logged and can be recovered by plugin in some cases. Needed for all current execution. |
| Virtual vs physical addressing | Stubbed / partially implemented | Optional `MMU` with identity mapping in `Memory` constructor; `MMU::TranslateAddress` | There is a simplified page mapper, but not IA-64 virtual addressing. Linux IA-64 needs regions, TLB/translation registers, and page attributes. Blocks Linux later. P1. |
| Region registers | Missing | No region register model found | IA-64 virtual addresses encode regions and require region identifiers. Missing support prevents Linux virtual memory correctness. Blocks Linux later. P1. |
| TLB / VHPT support | Missing | No IA-64 TLB, TR/TC, VHPT walker, or ITC/PTC instruction support found | Linux will program translations and expect faults/VHPT behavior. Missing support blocks real kernel execution after handoff. P1. |
| Page table assumptions | Stubbed | `MMU` is a simple virtual-page to physical-page map, not an IA-64 page table walker | Useful for emulator tests only. Kernel page tables will not be interpreted. Blocks Linux later. P1. |
| Execute permissions | Missing / risky | `Memory::Read` is used for instruction fetch; MMU read/write permissions exist but execute permission is not enforced on fetch | Incorrect execute permissions hide mapping bugs and kernel protection faults. Linux later. P2. |
| Endianness assumptions | Partially implemented / risky | Decoder slot extraction and memory load/store use host/little-endian `memcpy` | IA-64 Linux commonly uses little-endian, so this may be acceptable for current target. It is still implicit and host-dependent. Symptoms on a non-little-endian host would be catastrophic. Long-term completeness. P3. |
| Alignment faults | Missing | Memory reads/writes use `memcpy`; no alignment checks found | IA-64 has alignment fault behavior and unaligned access rules. Current code allows unaligned reads/writes if in bounds. Symptoms: missing faults and hidden bad code/data assumptions. Linux later. P2. |
| Unaligned access behavior | Stubbed | Same as above | Some IA-64 unaligned operations are explicit. Generic unaligned memory should fault or be handled architecturally. Linux later. P2. |
| Memory attributes | Missing | No cacheability, ordering, NaTPage, access rights, or MAIR-like attribute model found | Firmware/kernel may rely on UC/WB device memory semantics. Symptoms: incorrect device interaction or missed barriers. Linux later and platform completeness. P2/P3. |
| Device mapping | Partially implemented | `Memory::RegisterDevice`, device-first access checks | MMIO-style devices can be mapped, but no IA-64 I/O architecture or PCI config model was found. Linux later. P2. |

## 4. Exceptions and Interrupts

| Feature | Status | Evidence | Risk, symptoms, block, priority |
|---|---|---|---|
| Fault delivery | Missing / stubbed | Legacy unknown skip; plugin exception return; no IA-64 interruption vectors/state collection | IA-64 faults require rich state collection. Current behavior either skips, exceptions out, or performs diagnostic recovery. Symptoms: no page faults, illegal op faults, NaT consumption faults, or alignment faults. Blocks Linux later. P1. |
| Trap frame state | Missing | No real trap frame or interruption register save model found | Kernel cannot inspect IIP/IPSR/IFS/IFA/ISR/IIM/ITIR. Blocks Linux later. P1. |
| Interruption collection | Missing | `CPU::servicePendingInterrupt` and plugin service pending interrupt use synthetic AR/IP behavior | IA-64 interruption collection is architectural and complex. Current stubs can deliver a vector-like jump but not kernel-compatible state. Blocks Linux later. P1. |
| Break instruction handling | Stubbed | `BREAK` supports syscall immediate `0x100000`; otherwise effectively no full fault | Firmware/kernel break usage will not be delivered correctly. Symptoms: missed debug/syscall/trap behavior. Linux later. P1/P2. |
| External interrupt model | Stubbed / partially implemented | `BasicInterruptController`, CPU pending interrupt queue, plugin `checkInterrupts` | A generic interrupt queue exists, not IA-64 SAPIC/external interrupt architecture. Linux interrupt controller probing will fail or need synthetic support. Blocks Linux later. P1. |
| Timer interrupts | Partially implemented | `VirtualTimer` MMIO and callback to `BasicInterruptController`; `VirtualMachine::Run` ticks timer | Timer device exists and can raise vectors. It is not an IA-64 platform timer/ITC/ITM/SAPIC-compatible model. Linux later. P1/P2. |
| Machine check | Missing | No MCA/MCE model found | Linux/platform firmware may expect machine-check structures. Usually not first boot blocker, but important completeness. P3. |
| Illegal operation behavior | Stubbed / risky | Legacy skip in `src/core/cpu_core.cpp`; plugin `ExecutionResult::EXCEPTION` for unsupported instruction | Divergent behavior means errors may be hidden in one path and fatal in another. Linux needs real illegal-op fault delivery. Blocks debugging now and Linux later. P1. |
| Page faults | Missing | Simplified MMU returns false on unmapped/permission errors; no IA-64 page fault vector/state | Current boot may recover failed loads instead of faulting. Linux virtual memory cannot work without page faults. Blocks Linux later. P1. |

## 5. EFI / Firmware Emulation

### Implemented or partially implemented paths

| Feature | Status | Evidence | Risk, symptoms, block, priority |
|---|---|---|---|
| Synthetic EFI handoff region | Partially implemented | `VMManager::SetupEFIEnvironment` style code writes tables around `0x1FE00000` | Provides enough structure for bootloader entry. Correctness depends on table fields, descriptors, GP, and stubs. Blocks ISO boot now if wrong. P0. |
| EFI System Table | Partially implemented | `VMManager.cpp` writes system table, Boot Services, Runtime Services, and handles | Table headers and pointers exist. Coverage and CRC correctness are unknown. Symptoms include loader rejecting firmware tables. Blocks ISO boot now. P0. |
| Boot Services coverage | Stubbed / partially implemented | EFI service descriptors in `VMManager.cpp`; intercept handling in `IA64ISAPlugin.cpp` | `AllocatePool`, `FreePool`, `HandleProtocol`, and `SetWatchdogTimer` have useful or success behavior. Most services are generic success/unsupported or absent. Missing `GetMemoryMap`/`ExitBootServices` correctness blocks kernel handoff. P0. |
| Runtime Services coverage | Stubbed / partially implemented | Runtime table in `VMManager.cpp`; `GetVariable` handling in plugin | `GetVariable` handles BootCurrent, Boot0000, and language probes. Other runtime services are generic success or unsupported. Linux later may need runtime service table correctness. P1/P2. |
| ConsoleOut / SimpleTextOut | Partially implemented | Text output protocol table in `VMManager.cpp`; `OutputString` interception in plugin | Native log confirms OutputString calls and framebuffer/console mirroring. Missing full mode/query/set behavior. Good enough for diagnostics, not full firmware. P2. |
| GOP | Missing / unknown | Framebuffer exists, but no clear EFI GOP protocol implementation found | Modern EFI loaders may look for GOP. IA-64 elilo may rely more on SimpleTextOut, but Linux graphics handoff may expect framebuffer info. Could block some loaders or display. P1/P2. |
| Simple File System protocol | Stubbed / partially implemented | SimpleFS handle/protocol table and `OpenVolume` function descriptor in `VMManager.cpp`; plugin has OpenVolume intercept | Protocol pointer can be returned. Latest native log says `SimpleFS.OpenVolume calls=0`, so current loader path has not proven it. File operations behind root protocol are mostly unsupported. Blocks ISO boot/kernel file loading when loader attempts file access. P0. |
| File Protocol | Stubbed | `VMManager.cpp` sets most root file operations to unsupported stubs; Close/Flush success | EFI loaders need `Open`, `Read`, `GetInfo`, and often directory iteration to load kernel/initrd/config. Missing support likely blocks kernel handoff once SimpleFS is used. P0. |
| Block I/O | Missing | Block device classes exist, but no EFI Block I/O protocol exposure found | Loaders may use Block I/O or Device Path to find media. Missing protocol can prevent ISO filesystem access through firmware. P0/P1. |
| Device paths | Missing / stubbed | LoadedImage and handles are synthetic; no robust device path parser/model found | EFI loaders use device paths for image/media identity. Stubbed paths can break protocol discovery and loader config resolution. Blocks ISO boot now or kernel handoff. P0/P1. |
| LoadedImage protocol | Partially implemented | `VMManager.cpp` writes LoadedImage protocol fields; `HandleProtocol` recognizes GUID | Provides image base/size and system table-ish values. DeviceHandle/FilePath correctness is unknown. Symptoms include loader failing to locate its boot device. Blocks ISO boot now. P0. |
| HandleProtocol | Partially implemented / risky | Plugin recognizes LoadedImage and SimpleFS GUIDs; native log shows zero-GUID fallbacks at hard-coded callsites | Useful boot progress hack. Hard-coded zero-GUID fallbacks are risky and not general EFI semantics. Symptoms include wrong interface pointer for real GUID calls. Blocks ISO boot now if loader uses unsupported protocols. P0. |
| LocateHandle / LocateProtocol | Missing / stubbed | No strong implementation found; generic service behavior only | EFI loaders often enumerate handles/protocols. Missing enumeration can prevent disk/filesystem discovery. Blocks ISO boot/kernel handoff. P0. |
| GetMemoryMap | Missing / stubbed | Boot Services table includes service slot, but no real descriptor map handling found in plugin | Kernel handoff requires a valid memory map and map key. Without it, `ExitBootServices` cannot be correct. Blocks kernel handoff. P0. |
| ExitBootServices | Missing / stubbed | No real state transition/map-key validation found | Required before OS kernel takes ownership. Generic success can be dangerous if memory map/device state is wrong. Blocks correct kernel handoff. P0. |
| PE/COFF loading | Partially implemented | `PEParser` parses PE headers, maps sections, validates IA-64, handles entry descriptors | Current log confirms loader image parsing and entry. Missing relocation coverage and GP data correctness remain. Blocks ISO boot now when loader data references are wrong. P0. |
| PE/COFF relocations | Partially implemented / risky | `PEParser` handles DIR64 and selected IA-64 ELF-style relocations; native log shows REL64/FPTR counts | IA-64 relocation set is broad. Missing IMM64/GP-relative/function descriptor relocation forms can corrupt pointers and code immediates. Native log already suspects GP/data addressing. Blocks ISO boot now. P0. |
| Function descriptors | Partially implemented | EFI service function descriptors are written as `[code, gp]`; PE entry descriptor logic; plugin indirect-call handling | IA-64 ABI depends on descriptors. Current support is synthetic and incomplete. Wrong GP/code pairing causes calls to wrong code or wrong data segment. Blocks ISO boot now. P0. |
| GP setup | Partially implemented / risky | VM setup writes `r1` from parser `globalPointer`; native log warns GP outside image and mirrors sections | Correct `gp` is critical for almost every global access. Current warning indicates high risk. Symptoms include out-of-bounds loads/stores and corrupted strings/pointers. Blocks ISO boot now. P0. |

### EFI log evidence

The latest native log includes:

- PE image parse/mapping milestones.
- Entry descriptor handling with code entry and GP.
- Warning: `IA-64 entry descriptor GP=0x238000 is outside SizeOfImage=0x5e000`.
- ELF relocation coverage summary with `REL64` and `FPTR64` counts.
- Non-exec section mirror workaround at low memory.
- `HandleProtocol`, `GetVariable`, `OutputString`, and `AllocatePool` calls.
- Final milestone: EFI app returned before kernel handoff.
- Final milestone reports `SimpleFS.OpenVolume calls=0`.

The native error log includes several out-of-bounds load/store records and recovery near IPs such as `0xeac0`, `0xeb60`, and `0x3f30`, including suspicious addresses such as `0x3a002000650064`. This strongly suggests GP/data relocation/string pointer corruption or missing loader data setup, but the exact cause still needs instrumentation.

## 6. Platform Devices

| Feature | Status | Evidence | Risk, symptoms, block, priority |
|---|---|---|---|
| Framebuffer | Partially implemented | `FramebufferDevice`; VM creates BGRA32 framebuffer at `0xB8000000`; SimpleTextOut mirror draws text | Useful for text diagnostics and display. Not equivalent to EFI GOP or Linux framebuffer handoff. Symptoms: loader displays text but OS cannot discover graphics. Linux later/display. P2. |
| GOP | Missing | No clear GOP protocol table or mode structure found | EFI loaders/kernels may require GOP for graphics. Could block display or some boot paths. P1/P2. |
| Disk / raw image access | Partially implemented | `VirtualBlockDevice`, `RawDiskDevice`, `ISO9660Parser`, `FATParser`; host-side ISO bootloader extraction | Host can parse images and extract bootloader. Guest firmware cannot yet expose full disk/file services. Blocks kernel loading through EFI. P0. |
| ISO access from firmware | Stubbed / missing | ISO parser exists outside guest protocol path; FileProtocol operations unsupported | Current VM can find/load EFI app host-side, but loader cannot yet read kernel/config via full EFI file APIs. Blocks kernel handoff. P0. |
| Keyboard input | Missing / unknown | No clear EFI SimpleTextIn/keyboard device implementation found | Bootloader menus may need keyboard. Lack of input can block interactive loader paths or timeout handling. P1/P2. |
| Timers | Partially implemented | `VirtualTimer`; generic interrupt delivery | Useful synthetic timer. Not IA-64 ITC/ITM/SAPIC-compatible. Linux later. P1/P2. |
| RTC | Missing / unknown | No RTC device found | Linux/firmware may query time. Usually not first blocker. P3. |
| PCI assumptions | Missing | No PCI config space/bus model found | Linux hardware discovery expects platform bus/PCI or SAL/ACPI-like descriptions. Blocks broad OS boot after handoff. P2. |
| Serial/debug console | Missing / partial diagnostics | Logs and VM console exist; no clear guest UART/serial MMIO found | Kernel early console may not work without serial or firmware console handoff. Diagnostics gap. P1/P2. |
| Interrupt controller | Stubbed / partially implemented | `BasicInterruptController`; vector queue; no SAPIC model | Linux IA-64 expects platform interrupt controller behavior. Blocks interrupts after kernel starts. P1. |

## 7. Linux / Bootloader Compatibility

| Compatibility area | Status | Evidence | Risk, symptoms, block, priority |
|---|---|---|---|
| IA-64 EFI application entry | Partially implemented | VM sets `r32=ImageHandle`, `r33=SystemTable`; sets stack and GP; native log shows EFI app executes | Good boot milestone. Still fragile due to GP/function descriptor and relocation issues. Blocks ISO boot if corrupted. P0. |
| elilo/EFI loader protocol expectations | Partially implemented / missing | LoadedImage, SimpleTextOut, SimpleFS handles exist; FileProtocol/Locate/GetMemoryMap incomplete | EFI loaders typically need LoadedImage, DevicePath, SimpleFS/FileProtocol, variables, memory map, and ExitBootServices. Missing pieces likely explain no kernel handoff. P0. |
| Linux IA-64 firmware expectations | Missing / unknown | No PAL/SAL, ACPI, real memory map, region/TLB, RSE, or interruption model found | Linux IA-64 expects firmware/platform data and architectural CPU features. Even if loader handoff succeeds, early kernel will likely fail without these. Blocks Linux later. P1. |
| Current ISO reaches | Partially confirmed | Latest native log reaches EFI app execution and service stubs; older `output.log` shows many unknown instructions and missing boot path attempts | Confirmed: PE app starts and prints. Not confirmed: loader opens volume, reads kernel, or invokes kernel entry. Current ISO boot is not complete. P0. |
| Boot milestones confirmed | Partially implemented | PE parse, entry descriptor, GP setup, EFI table pointers, OutputString, GetVariable, AllocatePool, HandleProtocol | These are real progress points, but some include diagnostic workarounds. Continue instrumenting before broad rewrites. |
| Likely kernel handoff blockers | Missing / risky | Logs show no OpenVolume and top-level EFI app return; source shows missing FileProtocol, GetMemoryMap, ExitBootServices, Locate*, relocation coverage | Highest-risk blockers are firmware file services, memory map/exit boot services, GP/relocations, function descriptors, and RSE/call semantics. P0. |
| Direct kernel loading path | Stubbed / future | `VMManager.cpp` contains a direct-kernel TODO path | Not active evidence for current boot. Direct kernel load might bypass EFI loader but would not solve architectural gaps. Dangerous before loader handoff is understood. P2. |

## 8. Logging and Diagnostics

| Diagnostic area | Status | Evidence | Risk, symptoms, block, priority |
|---|---|---|---|
| Unsupported instruction logs | Partially implemented | Legacy `[SKIP]` logs in `src/core/cpu_core.cpp`; plugin detailed unsupported logs | Useful, but fallback divergence makes logs hard to interpret. Add consistent instruction counters by opcode/unit/template/IP. P0. |
| EFI milestone logging | Implemented / partial | Native log has `[EFI-MILESTONE]` and `[EFI-STUB]` lines | Very useful for boot progress. Needs more protocol-level argument/result logging and call chains. P0. |
| Out-of-bounds memory logs | Implemented / partial | Native error log records ld/st IPs, registers, addresses, and recovery | Valuable. Need correlate to relocation/GP/function descriptor source and last EFI call. P0. |
| Silent missing features | Risky | Many unimplemented execution paths no-op or generic success | Silent no-ops make false progress likely. Add counters for no-op decoded instructions and generic EFI success/unsupported returns. P0/P1. |
| Predicate/group tracing | Partial | Some plugin state exists; tests cover snapshots | Need optional trace of predicate source/destination, stop boundaries, and branch predicate reads around failures. P1. |
| RSE/CFM tracing | Missing / partial | `ALLOC` logs/tests exist, but no full frame trace | Add trace of `alloc`, `br.call`, `br.ret`, CFM/PFS, `r32-r39`, `b0`, `gp`, and synthetic call frame stack. P0/P1. |
| GP/global data tracing | Partial | Native log warns about GP and mirrors sections | Need explicit trace for all GP-relative-looking accesses, function descriptor loads, and relocated pointer ranges. P0. |
| EFI protocol call counters | Partial | Final milestone reports some counters | Add counters for every service slot and all FileProtocol methods, not only top-level service descriptors. P0. |
| Kernel handoff detection | Partial | Final milestone says returned before handoff | Add explicit detection for `LoadImage`, file read of kernel, ELF kernel parse, `ExitBootServices`, and branch to kernel entry. P0. |
| Interrupt/fault diagnostics | Missing / partial | Generic interrupt logs exist; no IA-64 fault frame logging | Add synthetic fault objects before implementing full delivery. P1. |

## Risk Register by Missing or Risky Area

| Area | Why it matters | Current source touch points | Symptoms | Block category | Priority |
|---|---|---|---|---|---|
| GP/data relocation correctness | IA-64 global data, literals, and function descriptors rely on correct `gp` and relocations. | `PEParser`, `VMManager.cpp`, `IA64ISAPlugin.cpp` | Wild pointers, out-of-bounds load/store, corrupted strings, wrong function descriptor targets. | Blocks ISO boot now. | P0 |
| EFI FileProtocol `Open`/`Read`/`GetInfo` | Loader must read kernel, initrd, and config from ISO/FAT media. | `VMManager.cpp` root file protocol table; `ISO9660Parser`, `FATParser`, block devices | Loader returns before handoff, cannot find kernel, no `OpenVolume`/file reads. | Blocks kernel handoff. | P0 |
| `GetMemoryMap` and `ExitBootServices` | EFI OS handoff requires a valid descriptor map and map key. | Boot Services table in `VMManager.cpp`; plugin EFI stubs | Loader cannot legally exit firmware; kernel sees bad memory map. | Blocks kernel handoff. | P0 |
| Locate/handle/protocol enumeration | Loaders discover boot devices and protocols through EFI enumeration. | `VMManager.cpp`; `IA64ISAPlugin.cpp` `HandleProtocol` only | Hard-coded zero-GUID fallbacks, no device discovery, protocol lookup failures. | Blocks ISO boot now. | P0 |
| Function descriptor and indirect call semantics | IA-64 calls use descriptors containing code and GP. | `PEParser`, `VMManager.cpp`, `IA64ISAPlugin.cpp`, branch execution | Calls enter wrong address or use wrong `gp`; top-level return to zero. | Blocks ISO boot now. | P0 |
| Register stack engine/backing store | IA-64 ABI and kernel depend on RSE. | `CPUState`, `CPU`, `InstructionEx::Execute`, plugin call frame stack | Lost arguments/locals, wrong returns, broken kernel entry. | Blocks Linux later; may affect EFI. | P0/P1 |
| Instruction fallback divergence | Skipping unknowns can hide bugs; hard exceptions can halt. | `src/core/cpu_core.cpp`, `src/isa/IA64ISAPlugin.cpp` | False progress, silent data corruption, unexplained halts. | Blocks debugging now. | P0 |
| IA-64 interruption state | Kernel faults/interrupts require IIP/IPSR/IFS/etc. | `CPU::servicePendingInterrupt`, plugin interrupt service | No real page faults, illegal op traps, break traps, external interrupts. | Blocks Linux later. | P1 |
| Region/TLB/VHPT model | Linux virtual memory depends on IA-64 translation architecture. | `MMU`, `Memory`, missing privileged insns | Kernel cannot enable paging/regions or handle translation misses. | Blocks Linux later. | P1 |
| PAL/SAL/ACPI/platform firmware | IA-64 Linux often relies on platform firmware calls/tables. | No clear implementation found | Kernel probing fails or lacks platform description. | Blocks Linux later. | P1 |
| Floating-point/media execution | Kernel/user FP state and optimized code require it. | `FTypeDecoder`, `CPUState` FP regs, sparse execution | Wrong FP state, unsupported instructions. | Linux later/completeness. | P2 |
| Alignment/NaT/speculation faults | IA-64 correctness relies on these. | `Memory`, `InstructionEx::Execute`, `CPUState` NaT storage | Hidden invalid memory access, missing traps, bad speculative load behavior. | Linux later. | P2 |

## A. Current Confirmed Boot Capability

Confirmed by source and latest native log:

1. The VM can create a synthetic IA-64 EFI execution environment.
2. The loader image can be parsed as IA-64 PE/COFF.
3. IA-64 entry descriptor handling exists and the image entry is invoked.
4. CPU argument registers are initialized for EFI application entry: `r32` image handle and `r33` system table.
5. `r1` GP setup exists when `PEParser` reports a global pointer.
6. The EFI app executes enough IA-64 instructions to call firmware stubs.
7. The app reaches `HandleProtocol`, `GetVariable`, `OutputString`, and `AllocatePool` stubs.
8. Text output is visible through native logs and mirrored console/framebuffer paths.
9. Some out-of-bounds load/store faults are detected and logged with IP/register context.
10. The run does not reach confirmed kernel handoff. The latest milestone says the EFI app returned before kernel handoff and reports `SimpleFS.OpenVolume calls=0`.

Not confirmed:

- Firmware file open/read of the kernel.
- Loader config parsing through EFI file APIs.
- `GetMemoryMap` correctness.
- `ExitBootServices` correctness.
- Branch to Linux kernel entry.
- Linux early boot execution.

## B. Top 10 Likely Blockers to Kernel Handoff

1. `FileProtocol.Open`, `Read`, `GetInfo`, and directory traversal are stubbed/unsupported.
2. `GetMemoryMap` does not appear to return a real EFI memory descriptor map with a valid map key.
3. `ExitBootServices` does not appear to implement real map-key validation or firmware state transition.
4. GP/global data relocation correctness is suspect; the native log explicitly warns about GP outside `SizeOfImage`.
5. IA-64 relocation coverage is incomplete, especially for instruction-immediate and GP-relative forms.
6. Function descriptor and indirect-call behavior is boot-specific and fragile.
7. `LocateHandle`, `LocateProtocol`, device paths, and protocol enumeration are missing or stubbed.
8. RSE/backing store behavior is not architectural and may corrupt call ABI state.
9. Unknown/unimplemented instruction behavior diverges between legacy CPU and plugin paths.
10. Out-of-bounds load/store recovery may hide the real loader failure and allow an early return instead of exposing the root cause.

## C. Top 10 IA-64 Correctness Gaps

1. Full register stack engine: BSP/BSPSTORE/RNAT/UNAT, dirty partition, spills/fills, and CFM/PFS semantics.
2. IA-64 interruption collection and delivery: IIP, IPSR, IFS, IIM, ISR, IFA, ITIR, vectors, and return-from-interruption behavior.
3. Region registers, TLBs, translation registers/cache inserts, VHPT, and translation faults.
4. Correct stop-bit/instruction-group semantics, including predicate timing.
5. Full function descriptor, GP, and ABI call semantics.
6. NaT propagation, speculative loads, advanced loads, ALAT, checks, and NaT consumption traps.
7. Privileged/system instruction coverage, including PSR and control register behavior.
8. Complete IA-64 relocation handling for PE/COFF/EFI loader images.
9. Floating point and media instruction execution, not just decode/storage.
10. Alignment, memory ordering, memory attributes, and device/cacheability behavior.

## D. Recommended Development Phases

### Phase 0: Freeze boot observability before broad changes

- Add counters/traces listed in section E.
- Make every generic EFI service return visible with service name, function descriptor address, arguments, status, and caller IP.
- Make every unsupported instruction visible with template, slot, unit, raw bits, predicate, and caller path.
- Make every diagnostic recovery visible as a counter and include last EFI call and current GP.

### Phase 1: EFI file path to kernel discovery

- Implement enough Simple File System and File Protocol for loader reads: `OpenVolume`, `Open`, `Read`, `GetInfo`, `Close`, directory entries, and file position.
- Back those methods with existing `ISO9660Parser`/`FATParser` rather than inventing a new filesystem.
- Implement protocol enumeration minimally enough for the current loader without hard-coded zero-GUID callsite behavior.
- Keep changes tightly logged and tested against the current ISO.

### Phase 2: EFI handoff contract

- Implement real `GetMemoryMap` descriptor generation and stable map-key behavior.
- Implement `ExitBootServices` state transition and failure behavior when map key is stale.
- Fill LoadedImage, DevicePath, and handle relationships enough for loader expectations.
- Add explicit kernel handoff detection.

### Phase 3: IA-64 ABI and loader correctness

- Fix GP/function descriptor handling using a single consistent call path.
- Expand IA-64 relocations based on actual loader image relocation records and failing IPs.
- Replace boot-specific call/return heuristics with a better descriptor-aware branch model.
- Strengthen `ALLOC`, CFM/PFS, and stacked-register behavior enough for loader/kernel entry.

### Phase 4: Early Linux architectural support

- Implement interruption state objects and page/illegal/alignment/break fault delivery.
- Add region/TLB primitives and enough privileged instructions for early kernel setup.
- Add timer/interrupt controller model compatible with the kernel path being targeted.
- Add PAL/SAL/ACPI or a documented synthetic alternative if the chosen Linux path can use it.

### Phase 5: Completeness and fidelity

- Fill out FP/media execution.
- Add memory attributes, ordering, and alignment fidelity.
- Add PCI/RTC/serial/keyboard/GOP as needed.
- Remove diagnostic recovery paths once real traps and firmware behavior are stable.

## E. Instrumentation to Add Before Coding

1. EFI service call trace:
   - Service table: Boot, Runtime, TextOut, SimpleFS, FileProtocol.
   - Fields: caller IP, descriptor address, resolved code, GP, first 6 args, status result.

2. EFI service counters:
   - One counter per service slot and per FileProtocol method.
   - Separate `genericSuccess`, `genericUnsupported`, and `realImplementation` counters.

3. Kernel handoff milestones:
   - `OpenVolume`, first successful kernel/config file open, total file bytes read, memory map returned, `ExitBootServices`, final branch to kernel entry.

4. GP and descriptor trace:
   - Log every indirect branch/call through a descriptor.
   - Log code pointer, GP pointer, old/new `r1`, and whether descriptor address is in image, EFI table, pool, or mirrored section.

5. Relocation audit output:
   - Count every relocation type found, applied, skipped, unsupported.
   - For unsupported relocations, include section, RVA, type, symbol/name if available, and target bytes.

6. Instruction coverage counters:
   - Per template, unit, opcode, `InstructionType`, unknown decoder reason, and execution fallback result.

7. Predicate/stop trace toggle:
   - Around branch and compare instructions, log predicate inputs/outputs, group snapshot values, stop boundaries, and current bundle address.

8. RSE/CFM trace:
   - Around `ALLOC`, `br.call`, `br.ret`, and `mov ar.pfs`, log CFM, PFS, `b0`, `r1`, `r32-r39`, synthetic call-frame depth, and pending call inputs.

9. Memory fault correlation:
   - For every out-of-bounds or failed load/store, include last 5 branch targets, last EFI service, current GP, current descriptor, and whether address resembles UTF-16 or image-relative data.

10. Silent stub detector:
   - Emit a warning once per stub kind, plus final counts, for `CHK_A`, generic EFI success, generic EFI unsupported, no-op FP, decoded-but-unexecuted branch forms, and skipped unknown instructions.

## F. Safe Features to Implement Now Without Destabilizing Boot

These are low-risk because they improve observability or fill currently stubbed firmware surfaces without changing core IA-64 execution semantics broadly:

1. Add instrumentation from section E.
2. Implement FileProtocol `GetInfo` for file size/name and filesystem info.
3. Implement read-only FileProtocol `Open`, `Read`, `SetPosition`, `GetPosition`, and `Close` backed by existing ISO/FAT parsers.
4. Add explicit EFI service-name decoding for every function descriptor address already written in `VMManager.cpp`.
5. Add relocation audit logging without changing relocation application yet.
6. Add a strict mode flag to fail on diagnostic load/store recovery after enough context is logged.
7. Add tests for current BOOTIA64.EFI protocol call sequences using synthetic service calls.
8. Add final boot summary counters for unknown instructions, generic EFI stubs, load/store recoveries, GP switches, descriptor calls, and FileProtocol calls.
9. Make unsupported FileProtocol methods return correct EFI status with logging instead of sharing anonymous generic stubs.
10. Add validation helpers for EFI table/function descriptor pointers before starting the app.

## G. Dangerous Features That Should Wait Until After Kernel Handoff

These are high-blast-radius areas that can change execution behavior broadly. They should wait until the loader-to-kernel path is visible and well instrumented:

1. Replacing the entire instruction execution model with full EPIC parallel/group semantics.
2. Rewriting branch/call/return behavior globally before descriptor and GP traces identify the failing path.
3. Implementing a full RSE/backing-store engine without loader regression tests.
4. Enabling real page faults and virtual translation by default before EFI file loading works.
5. Removing all diagnostic recovery paths before the root out-of-bounds causes are understood.
6. Adding broad PAL/SAL/ACPI emulation before knowing which kernel entry path is reached.
7. Implementing full interrupt delivery by architectural vectors before `ExitBootServices` and kernel entry are confirmed.
8. Introducing device enumeration/PCI models that change existing synthetic handle/protocol addresses.
9. Reworking PE/COFF loading layout before relocation and GP audits identify exact missing records.
10. Enabling strict alignment/NaT faults globally before the loader can complete its file and memory-map sequence.

## Closing Assessment

The next useful milestone is not "more IA-64 everywhere." It is a narrow, observable EFI loader path:

1. Preserve the current ability to enter BOOTIA64.EFI.
2. Make all EFI calls and descriptor/GP transitions visible.
3. Implement read-only firmware file access.
4. Return a real memory map.
5. Detect the first attempted kernel load and handoff.

Once that path is visible, IA-64 architectural work can be prioritized from concrete failing instructions and state transitions rather than from the whole architecture at once.
