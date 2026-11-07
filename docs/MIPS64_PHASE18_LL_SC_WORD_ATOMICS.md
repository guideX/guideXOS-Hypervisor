# MIPS64-18: LL/SC word atomics and authentic Linux continuation

## Result

**Outcome A.** The authentic Phase 17 instruction is `LL $v0,96($s0)`, not LWC2. Four-byte LL/SC execution now uses the existing reservation monitor, the accepted OpenWrt kernel retires the linked load and matching conditional store, and full-VM replay reaches the same next unsupported frontier. No COP2 or DI/EI behavior was emulated.

## Preflight and unchanged artifacts

- Repository: `D:\dev\guideXOS_Hypervisor_MIPS64`; branch: `MIPS64`.
- Starting HEAD: `32d523036028bc87a4ce97dfab23f5340fe1610c`.
- Upstream divergence before edits: `0 0`; worktree was clean.
- `.phase-mips64` identified MIPS64-17 as complete and MIPS64-18 as next.
- The inherited MIPS64 suite passed **44/44** with **16,096 checks** before implementation.
- The accepted Phase 17 trace was preserved byte-for-byte; SHA-256 remains `bc888a3d662cf9269aad197328091b3d87f3aee8d310cd5969b87b4d82025684`.
- Kernel: OpenWrt 24.10.4, Malta BE64, Linux 6.6.110, entry `0xffffffff80917670`, 256 MiB RAM, 11,013,776 bytes. SHA-256 remains `1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc`.
- The accepted Malta PROM/reset configuration was reused. No kernel or guest-memory patch was applied.

## Phase 17 classification correction

Phase 17 recorded this frontier at guest PC `0xffffffff801cbfd4`, fetch PA `0x001cbfd4`, with word `0xc2020060`. Its JSON trace reports opcode decimal 48, `rs=16`, `rt=2`, and immediate `0x0060`. Those are the raw fields for LL.

The earlier Phase 17 report called the word an “unsupported COP2 load encoding (LWC2)” and described it as a COP2 frontier. That label was an editorial mistake in the report prose. The Phase 17 decoder did not name the instruction: it returned generic `UnsupportedInstruction`, while the trace preserved the opcode and operand fields. No code or trace generator mislabeled the word as LWC2.

Independent decoding gives:

| Field | Value |
|---|---:|
| Word | `0xc2020060` |
| Primary opcode | `0x30` / 48 |
| `rs` | 16 (`$s0`) |
| `rt` | 2 (`$v0`) |
| Signed offset | +96 (`0x0060`) |
| Operation | `LL $v0,96($s0)` |

LWC2 uses primary opcode `0x32`, so it remains distinct and unsupported as COP2 load functionality. The MIPS64 Volume II, Revision 2.50 LL, SC, and DI definitions were checked against the raw fields. [MIPS64 Volume II Rev. 2.50](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf); [LLVM MIPS instruction definitions](https://android.googlesource.com/platform/external/llvm/+/9ad73e9/lib/Target/Mips/MipsInstrInfo.td).

## Authentic LL operands and data

The bounded production Malta replay reproduced the 150,572-retirement prefix and fetched the same instruction from guest memory. The LL data address is separate from the instruction fetch address:

| Observation | Value |
|---|---:|
| `$s0` before LL | `0xffffffff80b0f380` |
| Effective VA (`$s0 + 96`) | `0xffffffff80b0f3e0` |
| Translated data PA | `0x000b0f3e0` |
| Cache attribute | 3 (cached) |
| Data bytes, big-endian | `00 00 00 00` |
| Assembled word | `0x00000000` |
| `$v0` before LL | `0xffffffff80b0bb58` |
| Expected and actual `$v0` after LL | `0x0000000000000000` |
| Reservation before / after LL | inactive / active, size 4 |

The CPU sign-extends bit 31 of the loaded word to 64 bits. This observed positive word therefore produces zero. The reservation identifies backing 1, granule offset `0x00b0f3e0`, generation 0. LL retires normally; CP0 Count and VM cycles advance from 150,572 to 150,573. Cause, EPC, BadVAddr, and UART output remain zero.

## SC pairing and execution

The guest-memory instruction window is recorded in the trace. The immediate sequence is:

```text
0xffffffff801cbfd4  c2020060  ll    $2,96($16)
0xffffffff801cbfd8  24420001  addiu $2,$2,1
0xffffffff801cbfdc  e2020060  sc    $2,96($16)
0xffffffff801cbfe0  1040fffc  beq   $2,$0,-4
0xffffffff801cbfe4  00000000  nop
```

There is no memory access or exception between this LL and SC. The addition changes the zero word to one. The authentic SC uses the same effective VA and PA, stores the original low 32 bits (`0x00000001`), writes full-width result 1 to `$v0`, and consumes the reservation. Guest memory changes from `00 00 00 00` to `00 00 00 01`; SC retires at CP0 Count/VM cycle 150,575. The following branch sees success and does not take its retry path.

The static window also records a later LLD-guarded LL/SC sequence. The bounded execution record qualifies the first matching SC that the authentic path reaches; no second SC result is claimed unless recorded dynamically. Existing eight-byte LLD/SCD behavior remains covered by its inherited suite and by the authentic LLD/SCD checkpoint replay.

## Reservation monitor and CPU snapshots

`PhysicalAddressMap::ReadLinked` and `StoreConditional` now accept naturally aligned widths 4 and 8. Both widths use the same VM-owned dense-RAM backing IDs, 16-byte backing-relative granules, generation map, tracked writers, and reservation lock. Tokens include access size, so a word SC cannot consume an LLD reservation and SCD cannot consume an LL reservation. SC uses the source low word, performs a single conditional 4-byte store, returns 1/0 in the full-width target GPR, and consumes the CPU reservation on both success and failure.

The CPU supports the existing cached CCA policy (3 and 5). CCA 2 uncached accesses, ROM, MMIO, sparse RAM, and unmapped physical targets cannot establish linked reservations. TLB translation, privilege checks, and normal load/store exceptions run through the standard address path. The monitor does not simulate cache arrays, full cache coherence, or host-parallel SMP.

CPU serialization stays at v6 and 1,001 bytes: the v5 957-byte prefix is unchanged, followed by the CPU-local reservation. Active word reservations require four-byte alignment; active doubleword reservations require eight-byte alignment. Unsupported cache attributes, malformed reserved bytes, invalid widths, alignment, physical range, backing, granule, and generation data are rejected. Historical v3-v5 migration remains intact. Full-VM snapshots remain GXM64SNP v3 with CPU v6, memory v2, UART v4, and IRQ routing v1.

The tests cover active LL snapshot restore/replay, successful and failed SC, conflicting host and CPU1 writes, different-granule writes, cross-size mismatches, reservation replacement in both width orders, cross-VM isolation, invalid transactional restore, both endian modes, TLB mapping, user privilege, uncached access rejection, ROM/MMIO rejection, aliasing, misalignment, and sign extension. The inherited LLD/SCD tests continue to cover ERET/reset invalidation and the original eight-byte retry path.

## Authentic continuation and next frontier

The unchanged kernel is loaded into the accepted production Malta VM and executed from reset. The trace records:

- Total retired from reset: **151,572**.
- Total attempted steps: **151,573**. The extra attempt is the runner's next-frontier classification; the unsupported instruction is left unexecuted.
- Retired since Linux ELF entry: **151,555**.
- Retired beyond the former LL frontier: **1,000**, including the LL itself; 999 further instructions retired after LL.
- LL/SC, LLD/SCD, and SLLV active-checkpoint continuation replays all match the same frontier.
- Final UART output: 0 bytes; no architectural exception; Cause/EPC/BadVAddr remain zero; no valid TLB entries.

The next genuine unsupported instruction is:

| Field | Value |
|---|---:|
| Guest PC | `0xffffffff80b426c0` |
| Fetch PA | `0x000b426c0` |
| Word | `0x41606000` |
| COP0 `rs` / `rt` / `rd` | 11 / 0 / 12 |
| Classification | Valid MIPS64 Release 2 `DI $zero`, not implemented |
| Stop | `UnsupportedInstruction`, before executing DI |

Volume II Rev. 2.50 identifies `0x41606000` as DI and describes clearing Status.IE; `0x41606020` is EI. The decoder now recognizes these valid encodings as unsupported rather than incorrectly treating MFMC0 as reserved. DI/EI semantics are not implemented in this phase, and the guest has not completed boot.

The final JSONL is [`docs/artifacts/openwrt_malta_be64_ll_phase18.jsonl`](artifacts/openwrt_malta_be64_ll_phase18.jsonl), 792 records / 240,976 bytes, SHA-256 `97922e9832322aa4d4d5c2b7c7d5ef00729d196c42a25880c41a197ed41600ce`.

## Verification

- MIPS64 CTest: **46/46 suites passed**.
- Assertion checks: **16,333 passed** (16,096 inherited plus 175 in `Mips64LlScWordTests` and 62 in `Mips64WordReservationSnapshotTests`).
- Selected regressions: **6/6 passed** — `BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`, and `RiscVDecoderSmokeTest`.
- All 30 MIPS64 test targets and production `ia64emu` built successfully.
- Production `nm -C` confirms `Mips64Cpu::executeMemory`, `Mips64Cpu::opName`, and `Mips64Cpu::disassemble` are linked into `ia64emu.exe`.
- The unchanged kernel hash and Phase 17 trace hash were rechecked after execution.

## Limitations and MIPS64-19 scope

The reservation model is a deterministic dense-RAM monitor with a 16-byte granule, cached CCA 3/5 policy, and tracked guest/host writes. It is not full cache coherence or host-parallel SMP. Malta Linux boot completion remains unverified. DI/EI are identified but unsupported.

Recommended MIPS64-19 work: qualify only the MIPS64 Release 2 DI/EI Status.IE semantics and required interrupt hazard behavior, then continue the same unmodified kernel to the next frontier. Do not treat this bounded continuation as a boot-completion claim.
