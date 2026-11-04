# MIPS64-12 — ELF Loader, Malta PROM Handoff, and Linux Frontier

## Qualification result

**Outcome A — MIPS64-12 accepted.** `MIPS64_ELF_HANDOFF_VERIFIED` and
`AUTHENTIC_LINUX_ENTRY_VERIFIED` are achieved. `LINUX_BOOT_VERIFIED` is not:
the authentic Linux kernel stops at a precise unsupported MIPS64 instruction
before producing UART output.

The work started on branch `MIPS64` at
`9bee5d9fe2fbe3f3ef18fe2f203d2a05be6d144d`. The local and upstream branches
were aligned, the worktree was clean, and `.phase-mips64` marked MIPS64-11
complete and MIPS64-12 next. The MIPS64-5, -7, -8, -10 and -11 reports were
reviewed. The pre-edit baseline passed 37 inherited MIPS64 suites and 4,378
assertions.

## Artifact inventory

The pre-build search covered this repository, `E:\CodexArtifacts\guideXOS_Hypervisor`,
and `E:\CodexArtifacts\guideXOS_Hypervisor_MIPS64`. The MIPS64 artifact
directory did not exist. The general artifact directory contained Linux
3.2.78 source and `arch/mips/configs/malta_defconfig`, but no built MIPS
kernel, MIPS ELF guest, Malta U-Boot image, or YAMON firmware image. It also
contained unrelated PowerPC64 and RISC-V outputs; none were selected.

An authentic candidate was available from the official OpenWrt Malta BE64
release index. `vmlinux.elf` was downloaded to the ignored build directory
`build/mips64-12-artifacts/`; its SHA-256 matches the index entry:

| Artifact and full path | Size | Format / requirements | SHA-256 |
|---|---:|---|---|
| `D:\dev\guideXOS_Hypervisor_MIPS64\build\mips64-12-artifacts\openwrt-24.10.4-malta-be64-vmlinux.elf` | 11,013,776 bytes | ELF64, big-endian, `EM_MIPS`, `ET_EXEC`, MIPS64 Release 2, static Malta kernel | `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc` |

The release index lists the same image and digest under the `malta/be64`
target. The associated build configuration selects `CONFIG_TARGET_malta_be64`.
The kernel was used from the ignored build directory and is not included in
the commit. [OpenWrt release index](https://downloads.openwrt.org/releases/24.10.4/targets/malta/be64/),
[target build configuration](https://downloads.openwrt.org/releases/24.10.4/targets/malta/be64/config.buildinfo).

## Existing loader audit and supported ELF subset

The existing [`include/loader.h`](../include/loader.h),
[`src/loader/loader.cpp`](../src/loader/loader.cpp), and
[`docs/ELF64_LOADER.md`](ELF64_LOADER.md) describe an IA-64-specific loader
with IA-64 relocation and CPU initialization behavior. That implementation
remains unchanged. MIPS64 parsing is a separate byte-wise parser in
`src/mips64/Mips64ElfLoader.cpp`; it does not cast host structures over guest
bytes, call the IA-64 loader, or initialize IA-64 state.

The supported initial subset is:

- ELF64, current ELF version, System V or Linux OSABI, ABI version zero.
- `ET_EXEC`, `EM_MIPS`, guest-matching little- or big-endian encoding.
- MIPS64 Release 2, N64-style flags, and static loadable segments.
- Bounded program/section tables and staged bytes; at most 128 program
  headers, 4,096 section headers, and 256 MiB of file or staged segment data.
- Readable `PT_LOAD` segments with checked file bounds, `p_filesz <= p_memsz`,
  alignment congruence, non-overlapping virtual and physical ranges, and an
  executable, aligned entry point.

The parser rejects ELF32, `ET_DYN`/PIE, other machines or endianness, dynamic
linking/interpreters, relocation and dynamic symbol requirements, unsupported
MIPS architecture/ABI/ASE flags, unknown permission flags, malformed tables,
overflow, invalid notes, overlapping segments, and targets outside guest RAM
or inside reserved boot memory. PIC/CPIC flag bits are accepted only in this
static executable subset; they do not enable a dynamic linker.

Address placement follows checked MIPS64 mappings:

| ELF address form | Load target | CPU state |
|---|---|---|
| Sign-extended KSEG0 `0xffffffff80000000..9fffffff` | Low 29-bit physical address | No KX required |
| Sign-extended KSEG1 `0xffffffffa0000000..bfffffff` | Low 29-bit physical address | No KX required |
| XKPHYS with CCA 2 or 3 and reserved bits clear | Low 36-bit physical address | ROM enables Status.KX for an XKPHYS entry |
| TLB-mapped or unsupported virtual address | Rejected | No implicit masking |

`p_paddr == 0` is treated as unspecified. A nonzero `p_paddr` is accepted
when it is already the resolved physical address or when it is a supported
direct-map/XKPHYS alias that independently resolves to the same physical
address. Other conflicts are rejected. This handles the OpenWrt image, whose
`p_vaddr` and `p_paddr` both contain the high KSEG0 alias, while preserving
and validating all ELF64 address bits. QEMU Malta uses
`cpu_mips_kseg0_to_phys` as its ELF load callback, which provides the relevant
comparison for that image. [QEMU Malta loader and PROM implementation](https://github.com/qemu/qemu/blob/master/hw/mips/malta.c),
[QEMU MIPS system documentation](https://www.qemu.org/docs/master/system/target-mips.html).
The compatibility and KX address rules follow the
[MIPS64 Privileged Resource Architecture, revision 2.50](https://0x04.net/~mwk/doc/mips/MD00091-2B-MIPS64PRA-AFP-02.50.pdf).
The Malta board/CPU profile is also covered by the
[U-Boot QEMU Malta guide](https://docs.u-boot.org/en/latest/board/emulation/qemu-mips.html).

Segment bytes and zero-filled BSS are staged before live RAM changes. The
physical map transaction validates every destination as writable dense RAM,
rejects overlapping writes and ROM/MMIO before any device operation, prepares
replacement backing, then commits via no-throw swaps. Invalid ELF rejection
leaves the board, CPU entry state, RAM and scheduler untouched. The dense-RAM
transaction uses a full backing replacement so a multi-segment failure cannot
leave a partial image.

The loader records and validates `PF_R`, `PF_W` and `PF_X`, and the independent
guest checks its text/rodata/data layout. The current VM does not install
per-segment MMU permissions from those ELF flags; they describe and validate
the executable layout, while page-level protection remains future work.

## Malta PROM image and ROM handoff

The QEMU Malta PROM layout is used as the reference: a 16-entry array of
32-bit pointers at physical `0x2000`, followed by 16 bounded 256-byte string
slots. The argument and environment tables share the pointer array; the
environment begins after the two arguments. Strings start at `0x2040`.
Pointers are encoded in guest byte order and are sign-extended when placed in
64-bit CPU registers. QEMU’s source also uses 32-bit swapped pointer entries
and stores `memsize`, `ememsize` and `modetty0` name/value pairs. Linux Malta
uses `modetty0` when it configures the serial console. [QEMU Malta source](https://github.com/qemu/qemu/blob/master/hw/mips/malta.c),
[Linux Malta initialization](https://github.com/torvalds/linux/blob/master/arch/mips/mti-malta/malta-init.c),
[Linux MIPS firmware command-line interface](https://github.com/torvalds/linux/blob/master/arch/mips/fw/lib/cmdline.c).

The supported PROM values and exact guest-visible layout are:

| Item | Guest value |
|---|---|
| `$a0` | `2` |
| `$a1` | `0xffffffff80002000` (32-bit pointer table value `0x80002000`) |
| `$a2` | `0xffffffff80002008` (environment table pointer) |
| `$a3` | Guest RAM size in bytes |
| `argv[0]`, `argv[1]` | Kernel image name, command line |
| Environment | Optional `memsize`, `ememsize`, `modetty0` name/value pairs, then a null pointer |
| Default PROM reservation | Physical `0x2000..0x303f` |
| ELF guest completion word | Physical `0x4000`, value `0x47584f53` on success |

The synthetic Malta ROM remains the default profile. ELF mode is opt-in and
uses a generated 4 MiB target-endian ROM. At reset VA
`0xffffffffbfc00000`, ordinary MIPS64 instructions set `$a0`–`$a3`, construct
the full 64-bit entry address in `$t9`, optionally enable KX for an XKPHYS
entry, and transfer with `jr $t9` plus a real delay-slot instruction. It does
not set the PC from a host callback. The synthetic MIPS64-7 profile remains
unchanged; its completion convention is not overwritten by the ELF PROM
reservation.

This is only the direct-kernel argument/environment handoff. No YAMON service
vectors, shell, PCI devices, or other firmware services are claimed.

## Independently built guest and toolchain

`test_programs/mips64_elf/handoff.S` is assembled and linked independently of
the VM source. `linker.ld` produces three static ELF64 `PT_LOAD` segments:

| Segment | VA | Physical target | File / memory bytes | Flags |
|---|---:|---:|---:|---|
| Text | `0xffffffff80010000` | `0x10000` | 464 / 464 | R-X |
| Read-only data | `0xffffffff80011000` | `0x11000` | 33 / 33 | R-- |
| Data and BSS | `0xffffffff80012000` | `0x12000` | 32 / 36 | RW- |

Both images are ELF64 `EM_MIPS`, `ET_EXEC`, MIPS64r2, entry
`0xffffffff80010000`. Their `e_flags` are `0x80000007` (64R2, NOREORDER,
PIC and CPIC); they have no dynamic loader or relocation requirement.

The reproducible script uses the official LLVM 18.1.8 Windows x64 release,
Clang’s MIPS64 integrated assembler, and LLD 18.1.8. The installer is pinned
locally by SHA-256; the installer did not carry an Authenticode signature.
The build script refuses to overwrite generated ELFs unless `-Overwrite` is
passed. The source, script and linker script are committed; the compiler
installer, extracted toolchain and object files stay in ignored build paths.
[LLVM 18.1.8 release](https://github.com/llvm/llvm-project/releases/tag/llvmorg-18.1.8).

| Artifact and full path | Size | SHA-256 |
|---|---:|---|
| `D:\dev\guideXOS_Hypervisor_MIPS64\tests\fixtures\mips64_elf\guidexos_mips64_le.elf` | 74,752 bytes | `4d6dcd688aebddd6f9e3c6f41014b0743a0289298b0167671adf69a0e3bc402a` |
| `D:\dev\guideXOS_Hypervisor_MIPS64\tests\fixtures\mips64_elf\guidexos_mips64_be.elf` | 74,752 bytes | `d8ae832f6a6744a8679070195f0e4cf39ca2350b6d30348474e3693bddc66148` |
| `D:\dev\guideXOS_Hypervisor_MIPS64\build\mips64-12-toolchain\LLVM-18.1.8-win64.exe` (ignored build artifact) | 374,335,885 bytes | `94af030060d88cc17e9f00ef1663ebdc1126b35e16bebdfa1e807984b70abd8f` |

The guest validates argument registers and pointers, command-line content,
environment lookup and terminator, RAM size, BSS zeroing, initialized and
writable data, and a branch delay slot. It accesses the actual Malta NS16550
UART and writes the completion record in guest RAM. The exact console bytes
are `guideXOS MIPS64 ELF handoff OK\r\n`; the guest also changes its data word
to `0x66778899` and its BSS word to `0x47584f53`.

## Production VM execution and snapshots

Both endian images were loaded and executed through `VirtualMachine`, the
production MIPS64 CPU plugin, the Malta reset ROM mapping and the normal VM
scheduler. The LE and BE ROM traces begin at reset VA `0xffffffffbfc00000`,
physical `0x1fc00000`, with word `0x24040002`; they then reach the ELF entry
`0xffffffff80010000` through the encoded `jr` and delay slot. At entry the
registers contain the documented PROM values, KX remains clear for this
KSEG0 guest, and execution produces the exact UART message and RAM completion
values above.

Snapshots were taken after eight ROM instructions and again at the ELF entry.
Both snapshots restore and replay to identical CPU, RAM, UART, console and
scheduler state. A different PROM profile and a different valid ELF image
are rejected before changing the destination VM. ELF handoff snapshot identity
includes a stable 64-bit FNV content fingerprint, entry/KX state, endianness,
RAM size, ROM-related configuration and PROM settings. The actual program
bytes remain in the snapshotted RAM. The outer `GXM64SNP` stays v2; UART stays
v4, IRQ routing v1, and CPU serialization v5.

The deterministic trace files are committed under `docs/artifacts/`:

| Trace and full path | Bound / content | Size | SHA-256 |
|---|---|---:|---|
| `D:\dev\guideXOS_Hypervisor_MIPS64\docs\artifacts\mips64_elf_handoff_le.jsonl` | ROM reset through ELF entry, 17 retired instructions | 5,065 bytes | `030464de2b063779f53c766d67e8df2d0c306327cace93780f084da797349706` |
| `D:\dev\guideXOS_Hypervisor_MIPS64\docs\artifacts\mips64_elf_handoff_be.jsonl` | ROM reset through ELF entry, 17 retired instructions | 5,065 bytes | `030464de2b063779f53c766d67e8df2d0c306327cace93780f084da797349706` |
| `D:\dev\guideXOS_Hypervisor_MIPS64\docs\artifacts\openwrt_malta_be64_kernel_frontier.jsonl` | First 256 instruction records, explicit skipped-count record, and first unsupported instruction/status | 75,994 bytes | `e0f3b5f1c317daf8f66128a1473f968ba177ba89ce59c332cb7052429476b69f` |

Each trace records virtual PC, physical fetch where available, instruction
word, retirement/fault outcome, architectural exception fields, typed MIPS
fault, and VM cycle. The Linux trace has a 200,000-instruction hard cap and
stopped earlier; its gap record keeps the output small while the final event
records the exact frontier. No source symbols are inferred.

## Authentic Linux probe and observed frontier

The official OpenWrt Malta BE64 kernel passed the MIPS64 ELF loader with one
loadable segment. The ELF entry is `0xffffffff80917670`; its `p_vaddr` and
`p_paddr` are `0xffffffff80100000`, resolving to physical `0x100000`. The
segment is `PF_R|PF_W|PF_X`, with 11,008,432 file bytes and 11,310,036 memory
bytes. The probe used
256 MiB of big-endian guest RAM, command line `console=ttyS0,38400n8`, `$a0=2`,
the bounded 32-bit PROM tables in `$a1/$a2`, and `$a3=268435456`.

The ROM fetched its first instruction at reset VA `0xffffffffbfc00000` and
transferred through normal MIPS control flow. The first Linux instruction
fetched and retired was word `0x400c6000` at VA `0xffffffff80917670`, physical
`0x917670`. The run retired 150,041 instructions over 54 distinct PCs, then
stopped on:

```text
PC VA:       0xffffffff80917700
fetch PA:    0x917700
word:        0x000c643e  (llvm-objdump: dsrl32 $12, $12, 0x10)
MIPS fault:  UnsupportedInstruction (code 4)
detail:      valid MIPS64 SPECIAL encoding outside the implemented subset
CP0 Status:  0x00400080
Cause/EPC:   0 / 0
BadVAddr:    0
EntryHi/Lo0/Lo1: 0 / 0 / 0
valid TLB entries: 0
UART bytes:  0
VM cycles:   150041
```

This is a typed unsupported-instruction frontier, not an architectural MIPS
exception and not a missing-MMIO observation. The PC is in the KSEG0 direct
map, the fetch resolved to physical RAM, and no TLB entries were needed. The
guest had not reached UART setup. Linux did not boot, and no device response
was fabricated. A Release 2 `DSRL32` implementation with focused tests is a
candidate for MIPS64-13; after that, repeat this same bounded kernel probe.

## Tests, linkage, and remaining limits

Final qualification results:

- **38/38 MIPS64 CTest suites passed**, including all 37 inherited suites and
  `Mips64ElfHandoffTests`.
- **4,676 MIPS64 assertions passed**: 4,378 inherited plus 298 in the new
  handoff/loader/snapshot suite.
- The five selected IA-64 suites (`BasicTests`, `NewInstructionTests`,
  `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`) passed.
- `RiscVDecoderSmokeTest` passed.
- Production `ia64emu.exe` and `test_mips64_elf_handoff.exe` both linked.
- The default synthetic Malta, UART, timer/interrupt and snapshot regressions
  are included in the inherited MIPS64 qualification.

The ELF and PROM path remains deliberately narrow. It does not support ELF32,
dynamic linking, arbitrary relocations, TLB-mapped load targets, full YAMON,
the PCI/southbridge/storage/network devices, or Linux userspace. Segment
permission flags are validated but are not installed as guest MMU page
permissions. Linux boot remains unverified because the CPU stops at the
recorded unsupported instruction.

Recommended MIPS64-13 scope: implement and independently qualify MIPS64r2
`DSRL32`, rerun the OpenWrt Malta BE64 probe with the same cap and trace
format, then stop at the next typed CPU/platform frontier. Do not treat entry
or a later instruction count as Linux boot success.
