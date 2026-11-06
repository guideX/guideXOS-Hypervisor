#!/usr/bin/env bash
set -euo pipefail

commit=adc218676eef25575469234709c2d87185ca223a
source_dir="${1:-$PWD/linux-v6.12}"
build_dir="${2:-$PWD/linux-v6.12-build}"
artifact_dir="${3:-/mnt/e/CodexArtifacts/guideXOS_Hypervisor/phase-riscv18-linux-source}"

if [[ -e "$artifact_dir/Image" || -e "$artifact_dir/vmlinux" ]]; then
    echo "Refusing to replace existing Linux artifacts in $artifact_dir" >&2
    exit 2
fi

mkdir -p "$(dirname "$source_dir")" "$(dirname "$build_dir")" "$artifact_dir"
if [[ ! -d "$source_dir/.git" ]]; then
    git clone https://github.com/torvalds/linux.git "$source_dir"
fi
git -C "$source_dir" fetch --depth=1 origin "$commit"
git -C "$source_dir" checkout --detach "$commit"

export ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu-
make -C "$source_dir" O="$build_dir" defconfig
"$source_dir/scripts/config" --file "$build_dir/.config" \
    --enable NONPORTABLE --disable PORTABLE --disable SMP \
    --disable RISCV_ISA_C --disable FPU --disable RISCV_ISA_V \
    --disable EFI --disable BLOCK --disable NET
make -C "$source_dir" O="$build_dir" olddefconfig

export SOURCE_DATE_EPOCH=1731542400
export KBUILD_BUILD_TIMESTAMP="2024-11-14 00:00:00 UTC"
export KBUILD_BUILD_USER=codex KBUILD_BUILD_HOST=linux-riscv-fixture
make -C "$source_dir" O="$build_dir" -j8 V=1 Image vmlinux \
    2>&1 | tee "$artifact_dir/linux-v6.12-build-v1.log"

install -m 0644 "$build_dir/arch/riscv/boot/Image" "$artifact_dir/Image"
install -m 0644 "$build_dir/vmlinux" "$artifact_dir/vmlinux"
install -m 0644 "$build_dir/.config" \
    "$artifact_dir/linux-v6.12-rv64-nonefi.config"
(cd "$build_dir" && sha256sum .config vmlinux arch/riscv/boot/Image) \
    | tee "$artifact_dir/sha256-rebuilt.txt"
