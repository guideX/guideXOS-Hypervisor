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

common_flags=(-march=rv64ima_zicsr_zifencei -mabi=lp64 -mno-relax -mno-riscv-attribute)
"$cc" "${common_flags[@]}" -c -o "$output_dir/init.o" "$fixture_dir/init.S"
"$cc" "${common_flags[@]}" -nostdlib -nostartfiles -static \
    -Wl,-T,"$fixture_dir/init.ld" -Wl,--build-id=none \
    -Wl,-z,max-page-size=4096 -Wl,--no-relax \
    -o "$output_dir/init.elf" "$output_dir/init.o"
"$cc" "${common_flags[@]}" -c -o "$output_dir/child.o" "$fixture_dir/child.S"
"$cc" "${common_flags[@]}" -nostdlib -nostartfiles -static \
    -Wl,-T,"$fixture_dir/child.ld" -Wl,--build-id=none \
    -Wl,-z,max-page-size=4096 -Wl,--no-relax \
    -o "$output_dir/child.elf" "$output_dir/child.o"

"$readelf" -h -l -A -s "$output_dir/init.elf" > "$output_dir/init-elf-report.txt"
"$readelf" -h -l -A -s "$output_dir/child.elf" > "$output_dir/child-elf-report.txt"
(cd "$output_dir" && "$objdump" -d init.elf > init-disassembly.txt)
(cd "$output_dir" && "$objdump" -d child.elf > child-disassembly.txt)
"$python" "$fixture_dir/build_execve_initramfs.py" build \
    "$output_dir/init.elf" "$output_dir/child.elf" \
    "$output_dir/initramfs.cpio" "$output_dir/initramfs-manifest.json"
"$python" "$fixture_dir/build_execve_initramfs.py" validate "$output_dir/initramfs.cpio" \
    > "$output_dir/initramfs-validation.json"
"$python" "$fixture_dir/make_platform_dts.py" \
    "$base_dts" "$output_dir/initramfs-manifest.json" \
    "$output_dir/platform-initramfs.dts"
dtc -I dts -O dtb -o "$output_dir/platform-initramfs.dtb" \
    "$output_dir/platform-initramfs.dts"

cat > "$output_dir/build-provenance.txt" <<EOF
phase=RISCV-28
parent_source=tests/fixtures/riscv_linux_execve/init.S
parent_linker=tests/fixtures/riscv_linux_execve/init.ld
child_source=tests/fixtures/riscv_linux_execve/child.S
child_linker=tests/fixtures/riscv_linux_execve/child.ld
archive_builder=tests/fixtures/riscv_linux_execve/build_execve_initramfs.py
compiler=$($cc --version | head -n 1)
readelf=$($readelf --version | head -n 1)
objdump=$($objdump --version | head -n 1)
python=$($python --version 2>&1)
dtc=$(dtc --version)
compile_options=${common_flags[*]}
link_options=-nostdlib -nostartfiles -static -z max-page-size=4096 --no-relax --build-id=none
parent_entry=0x10000
child_entry=0x20000
EOF
(cd "$output_dir" && sha256sum init.elf child.elf initramfs.cpio \
    platform-initramfs.dtb > sha256.txt)
echo "Artifacts: $output_dir"
