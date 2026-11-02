# MIPS64-7 — Malta Bootstrap Profile

## Scope and repository preflight

MIPS64-7 adds an explicitly selected Malta-oriented bootstrap board to the
MIPS64-6 physical bus. It is a bounded board subset for board identification,
LED/display control, serial output, and cold boot. It is not a Malta-compatible
chipset and does not claim Linux/YAMON boot support.

The required preflight began on branch `MIPS64` at
`c2eef7ff8722fdd26b3b9417de4b408d038fe83f`, tracking `origin/MIPS64` and one
commit ahead. The only initial worktree item was the user's untracked `build/`
directory, which was preserved. `.phase-mips64` reported MIPS64-6 complete and
MIPS64-7 next. Both MIPS64-5 and MIPS64-6 acceptance documents were reviewed;
all 18 inherited MIPS64 CTest suites passed before MIPS64-7 changes.

While this work was in progress, the shared local branch advanced to
`f5fac615f166a940be138d1fe1b857096fa2b185`, which contains the board, FPGA,
reset, and acceptance-test implementation. Work continued on that branch tip;
this phase's integration, regression, documentation, marker, and follow-up
commit are recorded after it. No branch switching, worktree creation, history
rewriting, remote push, IA-64 source changes, or IA-64 `.phase` changes were
made. `E:\CodexArtifacts\guideXOS_Hypervisor_MIPS64` was checked and did not
exist, so no artifact was replaced.

## Hardware references and CPU configuration

Register semantics and non-overlapping FPGA/UART placement were checked
against [QEMU's Malta board implementation](https://github.com/qemu/qemu/blob/master/hw/mips/malta.c)
and its [MIPS system documentation](https://www.qemu.org/docs/master/system/target-mips.html).
Linux Malta definitions were cross-checked in
[`malta.h`](https://codebrowser.dev/linux/linux/arch/mips/include/asm/mips-boards/malta.h.html)
and the Malta platform sources. Reset vector and defined reset fields were
checked against the [MIPS64 Privileged Resource Architecture, Revision 2.50](https://0x04.net/~mwk/doc/mips/MD00091-2B-MIPS64PRA-AFP-02.50.pdf),
§5.2.4 and the exception-vector tables.

The board uses the repository's documented MIPS64r2 implementation subset
(SEGBITS=40, PABITS=36, CKSEG0/CKSEG1 and XKPHYS mapping, existing TLB/XTLB
behavior). It does not infer optional CPU capabilities from QEMU's 32-bit
Malta CPU model. The profile does not add an FPU, DSP ASE, microMIPS, or other
optional execution units. Board selection is VM configuration and does not
modify CPU decoding.

## Opt-in physical map

`VirtualMachine::configureMips64MaltaBootstrap()` explicitly enables the
profile. A normal MIPS64 VM remains on the MIPS64-6 flat-memory default unless
the physical map or Malta profile is explicitly selected. Guest RAM is the
VM's existing bounded RAM vector at physical address zero; its configured
size must end below the flash window at `0x1E000000`.

| Physical address | Size | Behavior |
| --- | ---: | --- |
| `0x00000000` | configured RAM | Existing VM RAM, read/write, retained across board reset |
| `0x1E000000` | 4 MiB, optional | Read-only flash reference view of the boot image |
| `0x1F000000` | `0x900` bytes | FPGA subset; aligned, native-endian 32-bit accesses |
| `0x1F000900` | `0x40` bytes | Existing CBUS NS16550; one-byte registers at 8-byte spacing |
| `0x1FC00000` | 4 MiB | Read-only executable reset ROM window |

When enabled, the flash and reset ROM views contain identical copies of the
same immutable backing image. Setting `mapFlashMirror=false` leaves the flash
address unmapped; it does not make the ROM programmable flash. Transactions
must fit wholly within one physical region. Unmapped addresses, invalid widths,
boundary crossings, and unsupported FPGA offsets fail explicitly. The FPGA
region stops at offset `0x900`, where the UART mapping begins, matching QEMU's
non-overlapping low FPGA and CBUS UART arrangement. The FPGA ASCII display is a
separate stateful output from the UART byte sink.

Construction validates the ROM and planned physical mappings before exposing
the board. If board construction fails after map setup, registered board
regions are removed and a newly enabled empty physical map is rolled back.
The board and its UART, FPGA, reset mailbox, and console sink are owned by the
VM instance. No mutable board state is global.

## Reset model

The PRA reset vector is virtual `0xFFFFFFFFBFC00000`; the accepted CKSEG1
translation maps it to physical `0x1FC00000`. After Malta profile initialization
and on `coldResetMips64Malta()`, each MIPS64 CPU begins at that vector. This is
an explicit board operation and does not use `setEntryPoint()` to imitate
architectural reset.

The implemented CPU reset state verified by tests is `Status=0x00400004`
(`BEV=1`, `ERL=1`, kernel privilege, `SR=0`), reset `EBase=0x80000000`,
zero `Cause`, zero `Count`/`Compare`, `Wired=0`, deterministic `Random`, and
invalid TLB entries. BEV, ERL, SR, and the reset vector match the PRA reset
definition. Count, Compare, and the remaining implemented CP0/TLB fields use
the core's deterministic emulator reset policy. The TLB is invalidated. The
generic MIPS64 VM reset remains at PC 0 and is otherwise unchanged.

Board reset preserves RAM and the ROM images. It resets the FPGA LED/display
and pending request, resets UART registers, and clears the per-VM console sink.
A guest write of the verified Malta `SOFTRES` command `0x42` records a request
in the board mailbox. The write completes first; the VM consumes it after the
current instruction retires, then resets all CPUs and devices through the VM
lifecycle and restarts at the ROM. Other values do not request a reset. The
request counter remains observable for diagnostics and bounded reset tests.
The modeled guest software reset uses the board's deterministic reset state;
it does not implement processor-specific debug/ErrorEPC preservation for a
physical MIPS Soft Reset exception.

## FPGA register subset

The FPGA device implements only these verified offsets. Transactions must be
aligned 32-bit native-endian loads or stores; bad widths and offsets fail
without partial effects.

| Offset | Register | Implemented behavior |
| ---: | --- | --- |
| `0x200` | SWITCH | Configurable guest-visible byte; read-only, default 0 |
| `0x208` | STATUS | `0x10` little-endian / `0x12` big-endian |
| `0x210` | JMPRS | Configurable guest-visible byte; read-only, default 0 |
| `0x408` | LEDBAR | Low byte stored and observable; reset value 0 |
| `0x410` | ASCIIWORD | Writes eight uppercase hexadecimal display characters |
| `0x418`–`0x450` | ASCIIPOS0–7 | Writes one low-byte display character per position |
| `0x500` | SOFTRES | `0x42` queues one deferred VM-scoped reset |
| `0x508` | BRKRES | Low byte read/write; reset value `0x0A` |

QEMU reports STATUS values from guest target endian and maps the CBUS UART
separately. The displayed ASCII state does not enter the serial sink. The
model omits all other FPGA/GPIO/I2C/chipset functions.

## UART, interrupts, and snapshots

The board reuses the MIPS64-6 NS16550 implementation at
`0x1F000900`, with register shift 3. Guest ROM instructions set DLAB, program
the divisor registers, configure line control, and write message bytes to
THR. The production VM's existing memory translation and bus route those
stores to the UART. UART ready status remains deterministic and transmit-only.

QEMU connects the Malta CBUS UART's interrupt output to CPU hardware input
INT2. In compatibility interrupt mapping, hardware INT0–INT5 correspond to
Cause.IP2–IP7, so hardware INT2 maps to Cause.IP4. This transmit-only UART has
no qualified interrupt source, so the profile does not assert INT2 or claim
UART interrupt delivery. It adds no PCI or 8259 controller. Existing
Count/Compare timer interrupt behavior is preserved and remains covered by the
inherited MIPS64 interrupt and production-VM suites.

CPU serialization remains accepted format v5. No board or device snapshot
format is claimed. A future complete VM snapshot must capture LEDBAR, ASCII
display, board input/reset state and pending reset, ROM identity and contents,
UART registers and output bytes, RAM mappings and contents, device identity
and registration, and pending interrupt lines under one VM boundary. Restoring
or replaying board state is deferred.

## Boot image and production VM execution

The reproducible fixture source is in
[`MaltaBootstrapBoard.cpp`](../src/mips64/MaltaBootstrapBoard.cpp): helper
functions build encoded MIPS64r2 instruction words, then serialize them in the
selected guest byte order without an external assembler at runtime. The ROM
window is zero-filled beyond the generated instructions. The separate RAM
payload is generated from its own encoded instruction list.

The ROM program starts at the reset vector, reads FPGA STATUS, writes LEDBAR
`0xA5`, writes ASCIIWORD and the eight `ASCIIPOS` characters `guideXOS`,
initializes the NS16550, and transmits this exact first message:

```text
guideXOS MIPS64 Malta bootstrap OK
```

It then executes `jr` to virtual `0xFFFFFFFF80001000`, which CKSEG0 translates
to physical RAM `0x00001000`. The independently generated RAM payload executes
guest `SB` instructions to the same UART and transmits:

```text
MIPS64 RAM payload running
```

The verified combined serial byte stream, with no inserted delimiter or
newline, is:

```text
guideXOS MIPS64 Malta bootstrap OKMIPS64 RAM payload running
```

The test constructs the configured `VirtualMachine`, maps its board devices,
loads only the RAM payload, then runs a bounded 512-cycle production VM
execution. The ROM is fetched from the CP0 reset vector through CKSEG1, its
FPGA and UART accesses use normal translation and physical-bus dispatch, and
the payload completes by writing marker `0x47584F53` to RAM at `0x2000`.
Tests check the output bytes, LED and ASCII state, UART divisor/line-control
registers, payload marker, and final payload PC. Little- and big-endian ROM,
payload, MMIO register, and output paths both pass.

## Test and regression results

The five focused targets are `Mips64MaltaMapTests`, `Mips64MaltaFpgaTests`,
`Mips64MaltaResetTests`, `Mips64MaltaBootTests`, and
`Mips64MaltaOwnershipTests`. They pass **5/5** with **166 assertion checks**.
Coverage includes default-profile opt-in, mapping and mirror behavior, failed
construction rollback, invalid-ROM diagnostics, reset state and ROM
fetch/immutability, FPGA values and invalid operations,
FPGA/UART coexistence, deferred guest reset and ROM re-entry, both-endian
production cold boot and payload handoff, separate VM devices, and shared
board mappings across CPUs. The 18 inherited suites retain 2,539 checks, for
**2,705 passing MIPS64 assertion checks** across all 23 suites.

All **18 inherited MIPS64 suites plus the five new Malta suites pass (23/23)**.
All **five selected IA-64 regressions pass (5/5)**: BasicTests,
NewInstructionTests, EfiMemoryMapTests, IA64SalTests, and VMCheckpointTests.
RiscVDecoderSmokeTest passes (1/1). The production `ia64emu` executable and
the production-linked Malta VM test target both build.

The full CTest inventory is 29/35 passing. Six legacy executables are absent
from the preserved build directory, so their tests are reported as Not Run:
SyscallTests, DebuggerTests, ProfilerTests, FuzzerTests, VMIsolationTests,
and ELFValidationTests. The separately built, unregistered
`test_isa_plugin.exe` reproduces its documented NaT assertion at
`tests/test_isa_plugin.cpp:1551`. These match the known MIPS64-6 baseline;
no IA-64 regression was introduced, and no IA-64 production source or phase
marker was changed.

The phase marker advances to `last_completed=MIPS64-7` and
`next_expected=MIPS64-8` after the focused and inherited acceptance suites
passed.

## Limitations and next phase

- This is a Malta-oriented bootstrap subset, not full Malta compatibility.
- There is no GT64120/Galileo controller, PCI, PIIX4, PIC, storage, VGA, NIC,
  firmware service layer, or CFI flash behavior.
- UART receive, FIFO behavior, baud timing, and interrupt delivery are absent.
- FPGA registers outside the table above are unsupported and fail explicitly.
- Board/device state is not included in VM snapshots or CPU serialization.
- Boot images are synthetic, and no Linux ISO or firmware image is supported.

Recommended MIPS64-8 scope: define a VM-boundary snapshot contract for
physical-bus mappings, ROM identity, FPGA/UART state, RAM, and console output;
stage and validate restore before publication; decide output replay semantics.
Any later external interrupt source should be integrated through a real CPU
hardware input path and independently tested, preserving Cause.IP mapping.
