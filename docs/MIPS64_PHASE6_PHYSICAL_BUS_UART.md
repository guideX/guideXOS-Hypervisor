# MIPS64-6 — Physical Address Map and NS16550 UART

**Status:** Outcome A — implementation and required execution evidence completed.
**Platform profile:** Malta-oriented UART register layout; this is not full Malta emulation.

## 1. Preflight

- Repository: `D:\dev\guideXOS_Hypervisor_MIPS64`.
- Branch: `MIPS64`; starting HEAD: `650581245ff42eba38c8012f33b122dca88715a0`.
- Upstream divergence at start: zero commits ahead and zero behind `origin/MIPS64`.
- Tracked worktree was clean. The untracked `build/` directory was preserved.
- `.phase-mips64` marked MIPS64-5 complete and MIPS64-6 next. The IA-64 `.phase` marker was read and left unchanged.
- MIPS64-3, MIPS64-4, MIPS64-4R1 and MIPS64-5 reports were reviewed. MIPS64-5 invariants retained: SEGBITS=40, PABITS=36, XKPHYS, compatibility CKSEG0/CKSEG1, paired-page 16-entry TLB, production scheduler, and CPU state format v5.
- Existing `Memory` used flat bounded backing and a legacy raw-pointer device registry. Its `tryDeviceRead/Write` fallback can continue to RAM when a mapped device declines a transaction, so it is not used by the new strict bus path.
- MIPS64 already translates and checks privilege before calling `IMemory` with the resulting physical address. Physical fetch failures already enter IBE; data failures enter DBE; alignment and architectural address errors remain separate.
- No reusable NS16550 device or MIPS64 physical-region registry existed. Existing generic VM console/timer devices remain separate and are not promoted to MIPS64 devices.
- The mandatory pre-change MIPS64 baseline passed all 14 registered suites.

## 2. Address and dispatch architecture

```text
Guest virtual address
        │
        ▼
MIPS64 privilege + segment/TLB/XKPHYS translation
        │  failure: AdEL / AdES / TLB exception
        ▼
36-bit guest physical address
        │
        ▼
Opt-in PhysicalAddressMap (one bounded region per transaction)
   ┌────┴────────────┬───────────────┐
   ▼                 ▼               ▼
 RAM             read-only ROM       MMIO
```

`VirtualMachine::enableMips64PhysicalAddressMap()` opts one MIPS64 VM into strict dispatch. It automatically registers that VM's existing RAM backing at PA `0` and lets the caller register ROM, sparse RAM, and MMIO. Without the explicit call, legacy flat `Memory` and IA-64 semantics remain in effect. The method refuses non-MIPS64 VMs and refuses configuration while a VM is running or paused.

| Region | Backing and access | Initial use |
| --- | --- | --- |
| RAM | Existing VM byte vector, shared with the map; read/write; widths 1, 2, 4, 8 | Low physical RAM at `[0, VM memory size)` |
| RAM | Owned dense vector, limited to 512 MiB per registration | Optional platform RAM |
| RAM | Sparse map with an explicit stored-byte capacity | High-address tests and sparse regions |
| ROM | Owned immutable byte vector; read-only; widths 1, 2, 4, 8 | Synthetic boot ROM fixture |
| MMIO | Shared-owned device backend; explicit permissions and widths | Optional UART registration |
| Unmapped | No backing | Deterministic bus error |

The registry holds at most 64 regions, sorts regions by base, rejects zero-size or overlapping mappings, checks arithmetic and the 36-bit physical range, and resolves every transaction to one region. A transaction that leaves its starting region fails before the target is called. Unmapped addresses never fall through to RAM. Sparse RAM reads zero for unwritten bytes and enforces a caller-supplied storage capacity.

`Memory::Read/Write` dispatch to the map only after opt-in. These calls are already downstream of MIPS64 CPU translation. The map checks region permissions and access width before making one backend call. ROM writes, invalid widths, boundary crossings, unmapped locations, and target errors return explicit `PhysicalBusError` codes. A rejected MMIO transaction causes no device call. The UART checks register validity and console capacity before committing a transmit byte.

## 3. Device ownership and VM lifecycle

The map owns each MMIO target with `shared_ptr`; the device does not depend on a CPU instance. The physical map belongs to `Memory`, and `Memory` shares its existing RAM vector with the registered RAM target, so there is no duplicate low-RAM allocation. Each VM gets an independent map and device instances. Multiple CPUs in one VM use the same shared memory and bus.

`Ns16550Uart::Reset()` clears UART registers and its bounded output sink. `PhysicalAddressMap::ResetDevices()` resets every mapped device. VM reset invokes this method. Stopping the VM or resetting an individual MIPS64 CPU leaves device state and emitted output intact. VM destruction releases the map's ownership; any external shared references remain valid.

The map, sparse RAM, ROM registration and UART state are not included in MIPS64 CPU serialization. `MemorySnapshot` continues to capture the legacy RAM vector and MMU state; it does not capture high sparse regions, ROM registrations, UART registers, or output bytes. Full VM snapshot support for this map is not claimed.

## 4. NS16550 subset

The device uses standard register indices and a configurable register shift. Accesses must be one byte and land on a register address. For the optional Malta-oriented profile, the physical base is `0x1F000900`, register shift is `3`, spacing is 8 bytes, and the eight-register window is 64 bytes.

| Register | Implemented behavior | Qualification and limits |
| --- | --- | --- |
| THR | Emits one byte when DLAB=0 | Sink capacity failure rejects the write without appending |
| RBR | Returns zero | No receive input or receive FIFO |
| IER | Stores and reads the written byte | No interrupt delivery |
| IIR | Returns `0x01` (no interrupt pending) | FCR is write-only at the same offset |
| FCR | Accepts and records the last written value for inspection | FIFO operation is not implemented |
| LCR | Stores/reads line control; DLAB bit selects divisor registers | No baud timing simulation |
| MCR | Stores/reads the register | No modem signals are modeled |
| LSR | Returns `0x60` (THRE and TEMT set) | Deterministic transmitter-ready state |
| MSR | Returns zero | No external modem inputs |
| SCR | Stores/reads the scratch byte | — |
| DLL/DLM | Read/write at offsets 0/1 while LCR.DLAB is set | No baud timing is derived from the divisor |

Interrupt signal inspection returns `false`; no IRQ is injected. Unimplemented IER bits are retained for inspection but do not create interrupts. Unsupported offsets, status-register writes, and non-byte accesses fail explicitly.

## 5. Guest UART mapping and execution evidence

The Malta-oriented mapping is registered by the platform setup, not by the CPU decoder. The guest uses the architecturally translated CKSEG1 address:

| Guest address | Translation | Physical region |
| --- | --- | --- |
| `0xFFFFFFFFBF000900` | CKSEG1 direct map, `PA = VA & 0x1FFFFFFF` | UART at `0x1F000900` |
| `0xFFFFFFFF80001000` | CKSEG0 direct map | Guest code at RAM PA `0x1000` |

The encoded MIPS64r2 guest constructs the UART address with `lui`/`ori`, constructs a CKSEG0 pointer to a NUL-terminated message in RAM, reads each character with `lbu`, polls `LSR.THRE` with `lbu`/`andi`/`beq`, and transmits using `sb`. It writes a completion marker to RAM after reaching the terminator. The VM runs this code through the production MIPS64 ISA plugin and `VirtualMachine` scheduler for a bounded 1,024-instruction budget. The test reads the sink afterward and compares the exact byte vector.

Observed guest output in both endian configurations:

```text
Hello from guideXOS MIPS64!
```

The same production guest-console suite also verifies two independent VMs cannot see each other's sink and that two enabled CPUs in one VM can each transmit through that VM's shared map. Endian-specific instruction images are encoded in the guest's byte order; the UART data register remains a single byte.

## 6. ROM and high-address qualification

A synthetic ROM at PA `0x1FC00000` is read and fetched through CKSEG1. The ROM guest writes a marker to RAM, then attempts to store to the ROM and receives DBE; the ROM bytes remain unchanged. ROM bytes are copied into immutable backing during host-side map setup.

The physical-bus suite registers sparse RAM above 4 GiB at PA `0x100000000` and executes an XKPHYS guest program from that region in little- and big-endian modes. The translated instruction fetch and data store/reload resolve to the registered high target. A sparse region also reaches the top physical byte `0xFFFFFFFFF`. Tests reject out-of-range accesses, a 64-bit arithmetic overflow, unmapped high PA, and cross-region transactions.

## 7. Tests and regression results

The four new CTest suites are:

| CTest suite | Scope | Assertion checks |
| --- | --- | ---: |
| `Mips64PhysicalBusTests` | Region registration, overlap/overflow, widths, boundaries, high PA, strict opt-in, XKPHYS, IBE/DBE vs AdEL | 50 |
| `Mips64UartTests` | Malta-oriented register mapping, transmit, ready polling, DLAB, binary output, unsupported behavior, capacity and reset | 36 |
| `Mips64PlatformBootstrapTests` | Synthetic ROM fetch, guest RAM write, ROM write fault | 12 |
| `Mips64ConsoleGuestTests` | Production VM guest output in both endians, VM/sink lifecycle, isolation, shared CPUs, destruction | 51 |

The pre-change baseline passed all 14 MIPS64 CTest suites with 2,390 assertion checks. The final regression results are recorded after rebuilding the affected targets below.

Final results:

- All 18 MIPS64 CTest suites passed (18/18).
- The 14 inherited MIPS64 suites retain 2,390 assertion checks; the four new suites pass 149 checks, for **2,539 checks passed and zero failed**.
- Selected IA-64 suites passed: `BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, and `VMCheckpointTests` (5/5).
- `RiscVDecoderSmokeTest` passed (1/1).
- The separately built, unregistered `test_isa_plugin` reproduces its known baseline NaT assertion at `tests/test_isa_plugin.cpp:1551`. The six repository CTest executables previously documented as unavailable also remain absent: Syscall, Debugger, Profiler, Fuzzer, VMIsolation, and ELFValidation. No IA-64 production source or IA-64 `.phase` was changed.
- Both the `ia64emu` production executable and the production-linked VM guest-console target built successfully.

CPU state serialization remains v5. No UART or physical map state was added to CPU blobs.

## 8. Limitations and next phase

- This profile implements only a Malta-oriented CBUS UART mapping. It is not full Malta, the separate Super I/O ports, the FPGA, a chipset, or firmware.
- UART receive, FIFOs, IRQ delivery, modem signaling, and baud timing are unsupported.
- The output sink is bounded in-memory byte storage. It does not block on or print to the host terminal.
- MIPS64 VM full snapshots do not yet capture physical map registrations, ROM, sparse RAM, UART registers, or output sink state. Output replay policy is undecided.
- A complete future snapshot must quiesce the scheduler and capture CPU states, RAM/MMU state, sparse regions, ROM identity, and each device plus output state under one consistent VM boundary. Restore must stage and validate all state before publication.

Recommended MIPS64-7 scope: define and implement a VM-level device snapshot contract for this bus, including sparse RAM and UART state, and decide whether restored console bytes are replayed or retained as historical output. Keep board interrupt routing and richer Malta bootstrap as separate, explicitly qualified follow-up work.
