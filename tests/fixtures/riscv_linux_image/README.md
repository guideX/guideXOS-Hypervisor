# Linux v6.12 RV64 Image fixture

This fixture manifest pins the authentic upstream Linux v6.12 raw RISC-V
`Image` at commit `adc218676eef25575469234709c2d87185ca223a`. The compiled
`Image`, `vmlinux`, full `.config`, source archive, and verbose build log are
retained outside the repository under
`E:\CodexArtifacts\guideXOS_Hypervisor\phase-riscv18-linux-source`. The
test reads the Image from that location by default; set the CMake cache entry
`RISCV_LINUX_IMAGE_PATH` to an independently retained copy with the same hash
when using a different artifact root.

The `Image` is 17,714,512 file-backed bytes. Its v0.2 header declares an
18,068,392-byte effective footprint, so the loader stages and zero-fills the
353,880-byte tail before committing the full `[0x80200000, 0x8133b3a8)` image
range. The 64-byte header's initial JAL targets offset `0xd4` (`_start_kernel`)
inside the authentic image. The separate 12 KiB FDT reservation remains at
`0x82200000`.

## Rebuild

Use Ubuntu 24.04 (WSL is suitable), the RISC-V GNU cross toolchain, DTC, and
the Linux build dependencies. The recorded build used GCC 13.3.0, GNU ld 2.42,
GNU Make 4.3, and DTC 1.7.0. From a fresh Linux case-sensitive filesystem:

```sh
git clone https://github.com/torvalds/linux.git linux-v6.12
git -C linux-v6.12 fetch --depth=1 origin adc218676eef25575469234709c2d87185ca223a
git -C linux-v6.12 checkout --detach adc218676eef25575469234709c2d87185ca223a
mkdir linux-v6.12-build
make -C linux-v6.12 O="$PWD/linux-v6.12-build" ARCH=riscv \
  CROSS_COMPILE=riscv64-linux-gnu- defconfig
linux-v6.12/scripts/config --file linux-v6.12-build/.config \
  --enable NONPORTABLE --disable PORTABLE --disable SMP \
  --disable RISCV_ISA_C --disable FPU --disable RISCV_ISA_V \
  --disable EFI --disable BLOCK --disable NET
make -C linux-v6.12 O="$PWD/linux-v6.12-build" ARCH=riscv \
  CROSS_COMPILE=riscv64-linux-gnu- olddefconfig
sha256sum linux-v6.12-build/.config
```

The configuration hash must match `configSha256` in `manifest.json`. The
recorded deterministic build environment and command are:

```sh
export ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu-
export SOURCE_DATE_EPOCH=1731542400
export KBUILD_BUILD_TIMESTAMP="2024-11-14 00:00:00 UTC"
export KBUILD_BUILD_USER=codex KBUILD_BUILD_HOST=linux-riscv-fixture
make -C linux-v6.12 O="$PWD/linux-v6.12-build" -j8 V=1 Image vmlinux
```

Keep build output and copied binaries outside the Hypervisor source tree.
Confirm the resulting Image and vmlinux hashes against `manifest.json` before
using them. The full configuration and build log are retained beside those
artifacts. The compiler command lines select
`-mabi=lp64 -march=rv64ima_zicsr_zifencei -mcmodel=medany`; compressed, floating
point, and vector instructions are disabled. This is build-profile evidence,
not a claim that the emulator can boot Linux fully.

The Linux-specific `platform.dts` is compiled with:

```sh
dtc -I dts -O dtb -o platform.dtb platform.dts
```

The source DTS and its compiled hash are pinned in the manifest. No Linux
kernel code or entry instruction is modified.
