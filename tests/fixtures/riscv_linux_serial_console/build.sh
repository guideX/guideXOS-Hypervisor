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

"$readelf" -h -l -s -A "$output_dir/init.elf" > "$output_dir/init-elf-report.txt"
(cd "$output_dir" && "$objdump" -d init.elf > init-disassembly.txt)
"$python" "$fixture_dir/build_serial_initramfs.py" build \
    "$output_dir/init.elf" "$output_dir/initramfs.cpio" \
    "$output_dir/initramfs-manifest.json"
"$python" "$fixture_dir/make_platform_dts.py" \
    "$base_dts" "$output_dir/initramfs-manifest.json" \
    "$output_dir/platform-initramfs.dts"
dtc -I dts -O dtb -o "$output_dir/platform-initramfs.dtb" \
    "$output_dir/platform-initramfs.dts"

cat > "$output_dir/build-provenance.txt" <<EOF
phase=RISCV-30
source=tests/fixtures/riscv_linux_serial_console/init.S
linker=tests/fixtures/riscv_linux_serial_console/init.ld
archive_builder=accepted RISCV-26 deterministic newc builder with RISCV-30 protocol metadata
compiler=$($cc --version | head -n 1)
readelf=$($readelf --version | head -n 1)
objdump=$($objdump --version | head -n 1)
python=$($python --version 2>&1)
dtc=$(dtc --version 2>&1)
compile_options=-march=rv64ima_zicsr_zifencei -mabi=lp64 -mno-relax -mno-riscv-attribute
link_options=-nostdlib -nostartfiles -static -z max-page-size=4096 --no-relax --build-id=none
entry=0x10000
EOF

(cd "$output_dir" && sha256sum "$fixture_dir/init.S" "$fixture_dir/init.ld" \
    init.elf initramfs.cpio platform-initramfs.dtb > sha256.txt)
echo "Artifacts: $output_dir"
