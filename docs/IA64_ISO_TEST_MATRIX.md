# IA-64 ISO Test Matrix

Status: 2026-08-07

This matrix keeps the modern/minimal IA-64 image separate from the historical Debian ELILO regression image. The Debian compatibility path is diagnostic-only and is gated by the exact loader SHA-256.

## ISO identities

| Image | Path | Size | SHA-256 |
|---|---|---:|---|
| Modern/minimal | `D:\dev\guideXOS_Hypervisor\iso\ia64\install-ia64-minimal-20240404T093405Z.iso` | 174,923,776 bytes | `71940AB5629E4C56F7D55B122193D12DFFF5784E0099E41C05A4F359278859AA` |
| Debian 7.11 regression | `D:\bkup\os\debian-7.11.0-ia64-DVD-1.iso` | 4,695,296,000 bytes | `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E` |
| Debian 7.11 netinst regression | `D:\bkup\os\debian-7.11.0-ia64-netinst.iso` | 270,692,352 bytes | `86BEB302354057C95EDCC92FCE6F524870AABBB7F1263BD2E2129FF5D7C11573` |

The extracted EFI loaders are different:

| Image | EFI source/path | Loader bytes | Loader SHA-256 | PE entry descriptor |
|---|---|---:|---|---|
| Modern/minimal | ISO9660 `/EFI/BOOT/BOOTIA64.EFI` | `0x51000` | `4C7AEB5232A4C3AC69AAE913D94C9ADC119C04F1BB4280C4C3F8BD76470E9574` | RVA `0x3a040` -> code `0x15430`, GP `0x0` |
| Debian 7.11 | `/boot/boot.img`, FAT `/EFI/BOOT/BOOTIA64.EFI` | `0x5c728` | `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA` | RVA `0x43ad0` -> code `0x1000`, GP `0x238000` |
| Debian 7.11 netinst | `/boot/boot.img`, FAT `/EFI/BOOT/BOOTIA64.EFI` | `0x5c728` | `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA` | RVA `0x43ad0` -> code `0x1000`, GP `0x238000` |

## Baseline and current traces

Runs used the `ia64_iso_matrix` runner with 512 MiB guest memory and a 300,000-cycle bound.

### Modern/minimal

The pre-fix trace initially misclassified raw `0x10000c000` at `0x36cc0` as an IP-relative branch and ran into zero-filled memory at `0x9bd6c0` until the cycle limit. After routing the recoverable El-Torito image through the common EFI handoff, the normalized pre-fix trace reached the same bad branch with EFI handoff metadata configured, but still did not reach EFI stubs or SimpleFS.

The shared decoder fix now identifies the instruction as `br.cond b6`. The loader follows real code through `0x36c00`, `0x35100`, `0x346e0`, `0xaf90`, `0x343b0`, and `0xac10`, then reaches the first concrete fault:

```text
IP 0xac80 slot 0: ld8 r14 = [r14]
baseBefore/target: 0x300905aad
GP (r1): 0x0
```

Recovery permits the trace to continue, but the resulting indirect `br.call b0 = b6` targets `0x300905a4d`, outside the 512 MiB guest. No unsupported instruction was observed. EFI handoff layout and the read-only boot-image backing store are configured; `SimpleFS.OpenVolume`, file protocol methods, and EFI stubs are not reached.

Result: the decoder fix materially improves the modern control-flow path. The remaining blocker is modern-image PE/COFF load-base, relocation, or GP-relative data placement - not the Debian ELILO compatibility behavior.

### Debian 7.11 regression

With `GUIDEXOS_EFI_DIAG_ELILO_ADDRESS_COMPAT=1`, the exact loader hash matches and the existing narrow shim applies its two known copies (`0x460` and `0x168` bytes, entry size `0x28`). It does not generalize to other loaders.

The shared decoder fix preserves the previously solved regression milestones:

```text
EFI app returned before kernel handoff
ConsoleOut.OutputString calls=6
SimpleFS.HandleProtocol returns=1
SimpleFS.OpenVolume calls=0
AllocatePool=5, FreePool=5
genericSuccessServices=6, genericUnsupportedServices=0
File.Open/Read/GetInfo/Close/SetPosition/GetPosition=0
recoveredLoadStores=1
clean top-level br.ret b0 halt at 197950 cycles
```

The first recovered fault remains the packed-UTF-16-looking pointer load at `IP 0x3f30 slot 1`, `ld2 r17 = [r14]`, target `0x66007500620020`. There is no unsupported instruction. The loader returns after protocol lookup and before `OpenVolume`/file I/O, matching the prior regression result.

### Debian 7.11 netinst regression

The netinst ISO has a BIOS-only El Torito catalog and no IA-64 EFI boot entry. The harness therefore falls back to `/boot/boot.img` and extracts `/EFI/BOOT/BOOTIA64.EFI`. That loader is byte-identical to the DVD regression loader and passes the unchanged exact SHA gate.

With `GUIDEXOS_EFI_DIAG_ELILO_ADDRESS_COMPAT=1`, the same narrow compatibility shim applies. The run preserves the Debian milestones: the recovered `ld2` fault remains at `IP 0x3f30 slot 1`, the EFI application returns cleanly at `197950` cycles, `SimpleFS.OpenVolume` remains `0`, and no FileProtocol file-I/O methods are called.

## Decision

- Keep `br.cond bN` register-target decoding as a general IA-64 fix; focused ISA and VM-manager tests pass.
- Keep the Debian ELILO address-compatibility shim exact-SHA gated.
- Do not apply or broaden that shim to the modern/minimal loader. Enabling the environment variable for the modern run produced loader identity only and no `ELILO-COMPAT` application.
- Do not claim the modern PE/COFF/GP blocker is shared with Debian: the two images fail at different behaviors, and Debian remains a useful regression path.

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

$env:GUIDEXOS_EFI_DIAG_ELILO_ADDRESS_COMPAT='1'
& .\cmake-build-refresh\bin\Debug\ia64_iso_matrix.exe `
  'D:\bkup\os\debian-7.11.0-ia64-netinst.iso' `
  --cycles 300000
```
