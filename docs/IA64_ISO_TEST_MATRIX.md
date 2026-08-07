# IA-64 ISO Test Matrix

Status: 2026-08-07

This is a three-ISO matrix with two distinct loader families:

| ISO case | Loader family | Role |
|---|---|---|
| Modern/minimal Gentoo IA-64 | Modern GRUB-family loader | Independent general IA-64 correctness probe |
| Debian 7.11 DVD | ELILO | ELILO regression path |
| Debian 7.11 netinst | The same ELILO binary as the DVD | Second media/layout regression point for the same loader family |

The Debian compatibility path is diagnostic-only and remains gated by the exact ELILO loader SHA-256. No separate netinst compatibility behavior is used.

## ISO identities

| Image | Path | Size | SHA-256 |
|---|---|---:|---|
| Modern/minimal Gentoo IA-64 | `D:\dev\guideXOS_Hypervisor\iso\ia64\install-ia64-minimal-20240404T093405Z.iso` | 174,923,776 bytes | `71940AB5629E4C56F7D55B122193D12DFFF5784E0099E41C05A4F359278859AA` |
| Debian 7.11 DVD | `D:\bkup\os\debian-7.11.0-ia64-DVD-1.iso` | 4,695,296,000 bytes | `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E` |
| Debian 7.11 netinst | `D:\bkup\os\debian-7.11.0-ia64-netinst.iso` | 270,692,352 bytes | `86BEB302354057C95EDCC92FCE6F524870AABBB7F1263BD2E2129FF5D7C11573` |

The Debian DVD and netinst contain the same exact EFI loader:

```text
source path: /boot/boot.img/EFI/BOOT/BOOTIA64.EFI
bytes:       0x5c728
SHA-256:     D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA
entry:       RVA 0x43ad0 -> code 0x1000, GP 0x238000
```

The modern loader is independent:

```text
source path: ISO9660 /EFI/BOOT/BOOTIA64.EFI
bytes:       0x51000
SHA-256:     4C7AEB5232A4C3AC69AAE913D94C9ADC119C04F1BB4280C4C3F8BD76470E9574
entry:       RVA 0x3a040 -> code 0x15430, GP 0x0
```

## Modern/minimal Gentoo IA-64

The A5 `addl` decoder bug is confirmed by the real loader slots. The old contiguous reconstruction produced false out-of-image values such as `0xa1138`, `0xa1140`, `0x1501e8`, and `0x1501f0`. The architectural reconstruction maps those accesses into the image, including:

```text
0x15440 slot 0: 0x12508970940 -> addl r37 = 0x250b8, r1
0x15430 slot 2: 0x12508980900 -> addl r36 = 0x250c0, r1
0x0ac20 slot 0: 0x12a80dd0380 -> addl r14 = 0x3a868, r1
0x0ac20 slot 1: 0x12a80de03c0 -> addl r15 = 0x3a870, r1
```

The original and mapped PE image contain identical bytes around the two requested targets:

```text
0x3a868: b8 50 02 00 00 00 00 00 c0 50 02 00 00 00 00 00
          u64 = 0x00000000000250b8
0x3a870: c0 50 02 00 00 00 00 00 b8 50 02 00 00 00 00 00
          u64 = 0x00000000000250c0
```

With `r1 = 0`, the corrected flow is:

```text
0xac20: r14 = 0x3a868, r15 = 0x3a870
0xac30: r14 = [0x3a868] = 0x250b8
0xac50: r15 = [0x3a870] = 0x250c0
0xac60: r14 = [0x250b8] = 0
0xac70: r14 = 0x60; [0x250c0] -> r42 = 0
0xac80: r14 = [0x60] = 0x6e75722065622074; then r14 += 0x118
0xac90: ld8 [0x6e7572206562218c] faults: first real blocker
0xaca0: only reached by the emulator's recovery continuation; indirect b6 is 0x300905a4d
```

The prior `0x300905aad` fault at `0xac80` disappears naturally. `0xac80` now executes successfully. The bogus `0x300905a4d` indirect target remains after recovery, but it is no longer the first fault. The previous first blocker was `0xac80`; the corrected architectural path reaches `0xac90` before recovery-observed `0xaca0`. The 300,000-cycle run stopped after 135 cycles.

Fresh modern run:

```text
.codex_tmp/ia64_matrix/20260807_205232/modern/modern.console.log
```

The run did not reach EFI services: `SimpleFS.OpenVolume=0`, all FileProtocol counts are zero, the EFI app did not return, and kernel-loading activity did not begin.

The earlier “malformed modern GRUB image” conclusion is false for the apparent GP-relative references listed above. Those references were produced by the incorrect guideXOS `imm22` decoder. The new `0xac90` blocker is a separate follow-up issue and is not evidence that the PE image is malformed.

## Debian 7.11 DVD and netinst

Both media were run separately with `GUIDEXOS_EFI_DIAG_ELILO_ADDRESS_COMPAT=1`, 512 MiB guest memory, and a 300,000-cycle bound. For both cases:

- the BIOS-only netinst catalog correctly falls back to `/boot/boot.img`;
- the extracted loader is `0x5c728` bytes and matches SHA-256 `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA`;
- the existing compatibility shim applies unchanged: two list-head cells, `0x460` global bytes, `0x168` image bytes, entry size `0x28`;
- `SimpleFS.OpenVolume=0` and all FileProtocol file-I/O counts remain zero.

The corrected A5 decoder changes the execution path before the former recovered `ld2` at `0x3f30`; therefore the old clean-return result is not preserved by this checkpoint. Both exact-loader runs now stop at the same new blocker:

```text
IP 0x6820 slot 2: br.call b0 = b6
b6 = 0xffff00000004
cycles = 14966
state = ERROR
```

The new blocker is identical for the DVD and netinst because the loader bytes are identical. It is a real post-fix regression against the previous 197,950-cycle clean-return baseline, not a hash-gate or media-selection difference. No Debian-specific decoder or compatibility behavior was added.

Fresh logs:

```text
.codex_tmp/ia64_matrix/20260807_205232/debian-dvd/debian-dvd.console.log
.codex_tmp/ia64_matrix/20260807_205232/debian-netinst/debian-netinst.console.log
```

## Decision

- Keep the A5 `addl` fix as a general architectural decoder/execution fix.
- Keep the existing indirect `br.cond bN` regression coverage; it remains in `test_isa_plugin`.
- Keep the existing exact-SHA ELILO compatibility shim unchanged and shared by both Debian media.
- Treat Gentoo as the independent general IA-64 probe.
- Treat Debian DVD and netinst as separate media cases for one exact ELILO loader family.
- Do not describe the modern image as malformed because of the former out-of-image GP-relative values.
- Do not call this checkpoint a clean three-ISO regression pass yet: the corrected decoder exposes a new Debian blocker at `0x6820` that needs a separate architectural investigation.

## Reproduction

```powershell
$env:GUIDEXOS_EFI_DIAG_ELILO_ADDRESS_COMPAT=$null
& .\cmake-build-refresh\bin\Debug\ia64_iso_matrix.exe `
  'D:\dev\guideXOS_Hypervisor\iso\ia64\install-ia64-minimal-20240404T093405Z.iso' `
  --cycles 300000

$env:GUIDEXOS_EFI_DIAG_ELILO_ADDRESS_COMPAT='1'
& .\cmake-build-refresh\bin\Debug\ia64_iso_matrix.exe `
  'D:\bkup\os\debian-7.11.0-ia64-DVD-1.iso' `
  --cycles 300000

& .\cmake-build-refresh\bin\Debug\ia64_iso_matrix.exe `
  'D:\bkup\os\debian-7.11.0-ia64-netinst.iso' `
  --cycles 300000
```
