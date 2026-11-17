#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: build.sh <artifact-output-dir> <base-platform.dts>" >&2
    exit 2
fi

fixture_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
output_dir="$(mkdir -p "$1" && cd "$1" && pwd)"
base_dts="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
cross="${CROSS_COMPILE:-riscv64-linux-gnu-}"
cc="${cross}gcc"
readelf="${cross}readelf"
objdump="${cross}objdump"
python="${PYTHON:-python3}"

"$cc" -march=rv64ima_zicsr_zifencei -mabi=lp64 -mno-relax \
    -mno-riscv-attribute -c -o "$output_dir/init.o" "$fixture_dir/init.S"
"$cc" -march=rv64ima_zicsr_zifencei -mabi=lp64 -mno-relax \
    -mno-riscv-attribute -nostdlib -nostartfiles -static \
    -Wl,-T,"$fixture_dir/init.ld" -Wl,--build-id=none \
    -Wl,-z,max-page-size=4096 -Wl,--no-relax \
    -o "$output_dir/init.elf" "$output_dir/init.o"

"$readelf" -h -l -A "$output_dir/init.elf" > "$output_dir/init-elf-report.txt"
(cd "$output_dir" && "$objdump" -d init.elf > init-disassembly.txt)
"$python" "$fixture_dir/build_initramfs.py" build \
    "$output_dir/init.elf" "$output_dir/initramfs.cpio" \
    "$output_dir/initramfs-manifest.json"

"$python" "$fixture_dir/build_initramfs.py" validate \
    "$output_dir/initramfs.cpio" "$output_dir/init.elf"

"$python" "$fixture_dir/make_platform_dts.py" \
    "$base_dts" "$output_dir/initramfs-manifest.json" \
    "$output_dir/platform-initramfs.dts"
dtc -I dts -O dtb -o "$output_dir/platform-initramfs.dtb" \
    "$output_dir/platform-initramfs.dts"

(
    cd "$output_dir"
    sha256sum init.elf initramfs.cpio platform-initramfs.dtb > sha256.txt
)
echo "Artifacts: $output_dir"
