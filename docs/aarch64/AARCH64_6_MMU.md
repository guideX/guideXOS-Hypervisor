# AARCH64-6 — Stage-1 MMU, Translation Tables, and Recoverable Faults

## Acceptance scope and starting state

This phase adds a bounded ARMv8-A AArch64 Stage-1 translation implementation
to the existing production CPU and ISA plugin. It does not add a second
interpreter, platform devices, firmware, or a boot loader.

At phase start the checkout was `D:\dev\guideXOS_Hypervisor_AARCH64`, branch
`AARCH64`, at `1589b1369edd2974b3e261ccf9fa20af3902f52a`, tracking
`origin/AARCH64` and one commit ahead of it. The working tree was clean and
`docs/aarch64/.phase` named AARCH64-5 as the last completed phase. The existing
AARCH64-5 acceptance gates passed before implementation.

The profile is non-secure EL0/EL1, A64, one CPU, Stage 1 only, little-endian,
4 KiB granules, and Normal RAM through the existing `IMemory` abstraction.
With effective `SCTLR_EL1.M=0`, the earlier flat-memory profile remains in
use. Unsupported controls and descriptor attributes are rejected explicitly.

## System registers and synchronization

The CPU implements `SCTLR_EL1`, `TCR_EL1`, `TTBR0_EL1`, `TTBR1_EL1`, and
`MAIR_EL1`, alongside the AARCH64-5 exception registers. EL0 cannot write
these EL1 translation controls; those accesses use the existing EL0 exception
path. Only `SCTLR_EL1.M` and `SCTLR_EL1.WXN` are accepted. Other SCTLR fields
return `Fault::UnsupportedFeature` rather than being silently ignored.

Writes to SCTLR, TCR, TTBR0, TTBR1, and MAIR update programmed state. Their
effective values change at `ISB SY` or another context synchronization event
(exception entry and ERET in this emulator). DSB and DMB execute in guest
instruction order for the single-CPU model and do not model cache hardware.
The decoder accepts architectural DSB/DMB domain/type options and only the
supported `ISB SY` form; reserved encodings are not treated as no-op barriers.

The guest transition in Program A writes translation tables while flat mode is
active, programs the translation registers, sets SCTLR.M, executes
`DSB ISHST; ISB`, and then fetches its next instruction through the identity
mapping at VA `0x1000` to PA `0x1000`. The exception vector page at VA `0x2000`
also has an identity mapping before activation.

There is no software TLB. Every instruction or data access walks the current
effective tables, so a guest descriptor change is visible on the next access.
The test guest uses STR through a separate mapping to replace a live L3 entry,
then loads from the same VA and observes the new PA's value. This has
deterministic semantics but no TLB performance or TLBI/cache invalidation
claim.

## Supported TCR and TTBR profile

`TCR_EL1.T0SZ` and `T1SZ` must each be 25 (39-bit VA region) or 16 (48-bit VA
region). The architectural 4 KiB encodings are `TG0=00` and `TG1=10`. The
implementation accepts the defined walk cacheability and shareability fields,
and the EPD0/EPD1 walk-disable controls. Setting EPD for the selected region
causes a translation fault. It fixes `A1=0` and 8-bit ASIDs (`AS=0`), rejects
TBI, HA/HD, DS, and other unsupported/reserved TCR bits, and accepts IPS sizes
32, 36, 40, 42, 44, and 48 bits. IPS values above 48 bits are not accepted.

TTBR0 selects the lower canonical range and TTBR1 the upper canonical range.
The walker does not fall back to TTBR0 for a noncanonical high address. This
profile uses ASID 0, CnP 0, and 4 KiB-aligned table bases. It rejects a root or
next-table address outside the selected IPS range. Translation-table reads
use physical `IMemory` addresses directly and never recursively enter the
guest virtual access path.

## Descriptor walk and address calculation

For a 39-bit VA, the walk begins at L1 and uses indexes VA[38:30], VA[29:21],
and VA[20:12]. For a 48-bit VA, it begins at L0 and uses VA[47:39] before the
same L1/L2/L3 indexes. Each index selects one of 512 little-endian 64-bit
descriptors. Walk depth is bounded to four levels.

| Descriptor level | Supported result | Mapping size | Output address |
| --- | --- | ---: | --- |
| L0 | Table only | — | next-level table base |
| L1 | Table or block | 1 GiB block | aligned output base + VA[29:0] |
| L2 | Table or block | 2 MiB block | aligned output base + VA[20:0] |
| L3 | Page | 4 KiB page | aligned output base + VA[11:0] |

Invalid and wrong-level descriptor types produce level-specific translation
faults. Misaligned block outputs and addresses above TCR.IPS produce
address-size faults. Unsupported PA extensions, reserved/unsupported leaf
attributes, and NSTable in the non-secure profile are rejected. Table
descriptors apply APTable, PXNTable, and UXNTable restrictions cumulatively;
architecturally ignored table bits remain ignored.

Leaf `AttrIdx` selects one byte of MAIR_EL1. This profile recognizes `0xFF`
(Normal write-back, read-allocate, write-allocate) and `0x44` (Normal
non-cacheable). Other attributes, including Device memory, are rejected when
used by a mapping. Leaf shareability values 01 are rejected. AF must already
be set; the emulator does not perform hardware AF/dirty-bit updates.

The walker checks the selected IPS size. The CPU then checks whether the
translated output span is backed by guest RAM. A valid translation to an
unbacked PA becomes a synchronous external abort with the guest VA in FAR;
page-table reads outside backing memory become external aborts on a table walk.

## Permissions and guest accesses

AP[1:0] implements privileged/user access and read-only/writable permissions.
EL0 data and instruction access requires AP user access; writes are rejected
for read-only leaf or APTable-restricted mappings. Instruction access is
distinct from data access: UXN blocks EL0 execution, PXN blocks EL1 execution,
and SCTLR.WXN blocks EL1 execution from writable mappings. EL1-only, RO, UXN,
and PXN behavior is checked on every relevant access.

The translation helper is used by instruction fetch and every implemented
memory form: literal loads, unsigned immediate loads/stores, pairs, unscaled
forms, pre/post-indexed forms, and SP-based accesses. Ordinary unaligned
Normal-memory accesses retain the old behavior. Multi-page accesses are split
at 4 KiB VA boundaries and each segment can translate to a separate PA.

Loads translate and bounds-check all spans before reading into temporary
bytes, then commit architectural destination registers only on success. A
later-page translation/permission failure therefore leaves all load
destinations unchanged. Stores translate and bounds-check all spans before
the first write, so a later-page fault does not modify the earlier page. The
tests include both a crossing LDP with unchanged destination registers and a
crossing pre-index STP with unchanged memory and base register on a second-page
fault. They also change a live descriptor twice and confirm each new PA is
used by the next load.

The generic `IMemory` interface does not offer transactional writes: a backend
that throws after mutating bytes can still leave a partial write across
physical segments. Such a backing-store failure is reported as an external
abort; atomicity is not claimed for that backend failure case.

## Synchronous aborts and vector dispatch

Instruction and data access aborts use the existing VBAR_EL1/ERET mechanism.
The supported exception classes are IABT lower/current EL (`0x20`/`0x21`)
and DABT lower/current EL (`0x24`/`0x25`). ESR_EL1 sets IL for A64 and carries
the walker FSC, WnR for data writes, and EA for synchronous external aborts.
S1PTW remains RES0 because it describes a Stage-2 fault during a Stage-1 walk
and Stage 2 is not implemented. FAR_EL1 receives the faulting virtual
address; ELR_EL1 initially receives the faulting instruction address;
SPSR_EL1 holds the prior PSTATE. Permission, access-flag, translation, and
address-size faults use their corresponding level-specific FSC values.

Representative exact syndromes verified by the tests are:

| Fault | ESR_EL1 | FAR_EL1 | ELR_EL1 at exception entry |
| --- | ---: | ---: | ---: |
| Current-EL IABT, L1 table-walk external abort | `0x86000215` | `0x1000` | `0x1000` |
| Current-EL IABT, translated PA outside RAM | `0x86000210` | `0x1000` | `0x1000` |
| Same-EL L3 translation DABT | `0x96000007` | `0x5000` | faulting LDR at `0x10F8` |
| Lower-EL L3 permission DABT | `0x9200000F` | `0x9000` | faulting LDR at `0x6008` |
| Lower-EL L3 write-permission DABT | `0x9200004F` | `0xA000` | faulting STR at `0x600C` |
| Lower-EL L3 UXN IABT | `0x8200000F` | `0xB000` | faulting fetch at `0xB000` |

The handlers inspect ESR/FAR/ELR with guest MRS instructions and return using
guest ERET. An unmapped/inaccessible vector cannot recursively enter itself:
the first failed vector instruction fetch stops with `Fault::TranslationFault`
and a bounded diagnostic. The pending-vector dispatch bit is included in the
v3 CPU record so this stop behavior survives a CPU checkpoint.

## Integrated guest execution

The three guest programs run through the production `AArch64ISAPlugin` and
`Cpu`, with guest instructions building the tables and handling faults.

### Program A — enable and use translation

Program A starts at EL1 with M clear. It writes the L1/L2/L3 descriptors using
real MOVZ/MOVK/STR instructions, sets MAIR, TCR, TTBR0, and VBAR with MSR, sets
SCTLR.M, executes DSB ISHST and ISB, then continues under translated fetch.
The 61-instruction run advances every PC from `0x1000` through the completion
PC `0x10F4` and runs twice from identical RAM/CPU initial conditions.

After activation, the guest loads `0x1122334455667788` through VA
`0x4000 -> 0x8000`, then stores `0xBEEF` through a different VA
`0x9000 -> 0xD000`. Tests verify the programmed/effective registers, those
independently expected translations, X12/X13/X14, unchanged load-page bytes,
the stored `EF BE` bytes at PA `0xD000`, matching CPU checkpoints, and matching
guest RAM. No unexpected exception occurs.

### Program B — recover from a missing page

Program B issues an EL1 LDR from unmapped VA `0x5000`. It enters the same-EL
Data Abort vector at VBAR+`0x200` (`0x2200`). Six real handler instructions
read FAR/ESR/ELR, add four to the saved ELR to skip the faulting load, and ERET.
The four program instructions plus six handler instructions reach completion
PC `0x1104` in 10 steps. The handler observed `ESR_EL1=0x96000007`,
`FAR_EL1=0x5000`, and initial `ELR_EL1=0x10F8`; execution resumes at `0x10FC`
and finishes at EL1. The complete sequence is repeated in a second independent
plugin after restoring the v3 CPU checkpoint with matching RAM.

### Program C — EL0 permissions

Guest instructions set ELR_EL1 to VA `0x6000`, set SPSR_EL1 to EL0t, and ERET.
EL0 successfully reads VA `0x7000 -> 0xB000`, writes VA `0x8000 -> 0xC000`,
and reads an RO mapping. Its load from EL1-only VA `0x9000` takes a lower-EL
DABT; the handler confirms the syndrome and skips that instruction. Its store
to RO VA `0xA000` takes a lower-EL DABT with WnR set; the physical sentinel at
PA `0xE000` remains unchanged. Finally, a RET to UXN VA `0xB000` takes a
lower-EL IABT. The handler selects EL0 continuation `0x6020` and ERET resumes
there. The guest returns to EL0 at completion PC `0x6028`.

## CPU checkpoint compatibility

The v3 CPU-state format is 400 bytes. It appends the programmed and effective
SCTLR/TCR/TTBR0/TTBR1/MAIR values to the 320-byte v2 base and uses the former
base reserved word at offset 316 for the exception-vector pending bit (bit 0;
all other bits must be zero). The full serialized record contains no host
pointers. Deserialization validates the full length, version, reserved bits,
PSTATE, VBAR alignment, SCTLR supported fields, and active TCR values before
applying any state.

The 280-byte v1 and 320-byte v2 layouts remain accepted. They restore with
translation registers/effective state zero and translation disabled. CPU
state does not include guest RAM or page tables; an executable VM checkpoint
must pair it with the matching memory image.

## Validation evidence

Windows/Ninja build command:

```powershell
cmake --build build --target test_aarch64_foundation test_aarch64_memory_branch test_aarch64_pc_stack test_aarch64_compiler_stack test_aarch64_exception_levels test_aarch64_mmu test_efi_memory_map test_ia64_sal test_riscv_decoder
```

Direct AArch64 test commands and checks:

| Command | Checks | Result |
| --- | ---: | --- |
| `.\build\bin\test_aarch64_foundation.exe` | 222 | pass, 0 failures |
| `.\build\bin\test_aarch64_memory_branch.exe` | 1,304 | pass, 0 failures |
| `.\build\bin\test_aarch64_pc_stack.exe` | 939 | pass, 0 failures |
| `.\build\bin\test_aarch64_compiler_stack.exe` | 5,725 | pass, 0 failures |
| `.\build\bin\test_aarch64_exception_levels.exe` | 326 | pass, 0 failures |
| `.\build\bin\test_aarch64_mmu.exe` | 530 | pass, 0 failures |

The focused totals are 9,046 AArch64 checks with no failures. The required
registered regression command is:

```powershell
ctest --test-dir build -R '^(AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|AArch64ExceptionLevelTests|AArch64MMUTests|EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest)$' --output-on-failure
```

It passed 9/9 suites. The selected EFI memory-map and IA-64 SAL suites passed;
the selected RISC-V decoder smoke test passed. `git diff --check` is also part
of final validation. The requested build emitted no new AArch64 compiler
warnings.

## Explicitly unsupported

Stage 2, EL2/EL3, secure state, AArch32, 16 KiB/64 KiB granules, 52-bit VA/PA
extensions, ASIDs other than zero, CnP, TBI, hardware AF/dirty-bit updates,
Device memory, unsupported cacheability attributes, hardware caches, TLBs,
TLBI, GIC/IRQ delivery, timers, SMP, MMIO devices, Linux/UEFI boot, and
virtualization extensions remain out of scope. Unsupported translation
controls fail explicitly rather than silently falling back to identity
mapping.

## Recommended AARCH64-7 milestone

Add architecturally scoped TLBI operations and a software TLB with explicit
ASID policy and invalidation tests, while preserving the current no-cache
reference mode. Keep cache-maintenance instructions, richer translation
features, and platform devices as separate later work.

## Architectural references

- [Arm Architecture Reference Manual for A-profile, DDI 0487](https://developer.arm.com/documentation/ddi0487/latest)
- [Arm Learn the Architecture: Memory Management](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/LearnTheArchitecture-MemoryManagement-101811_0100_00_en.pdf)
- [Armv8-A Address Translation User Guide](https://documentation-service.arm.com/static/5efa1d23dbdee951c1ccdec5)
- [Arm Learn the Architecture: Armv8-A Memory Model](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/Armv8-A%20memory%20model%20guide.pdf)

The TG0/TG1 encodings, 39/48-bit walk levels, descriptor fields, permission
behavior, and FSC/ESR definitions were checked against Arm's A-profile
architecture material. The bounded behavior above intentionally does not
claim support for omitted architectural features.
