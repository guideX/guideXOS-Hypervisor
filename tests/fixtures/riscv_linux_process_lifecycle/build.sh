#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: build.sh <artifact-output-dir> <base-platform.dts>" >&2
    exit 2
fi

fixture_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
output_dir="$(mkdir -p "$1" && cd "$1" && pwd)"
base_dts="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
common_fixture_dir="$(cd "$fixture_dir/../riscv_linux_initramfs" && pwd)"
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

"$readelf" -h -l -A -s "$output_dir/init.elf" > "$output_dir/init-elf-report.txt"
(cd "$output_dir" && "$objdump" -d init.elf > init-disassembly.txt)
"$python" "$common_fixture_dir/build_initramfs.py" build \
    "$output_dir/init.elf" "$output_dir/initramfs.cpio" \
    "$output_dir/initramfs-manifest.json"
"$python" - "$output_dir/initramfs-manifest.json" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
manifest = json.loads(path.read_text(encoding="utf-8"))
manifest["phase"] = "RISCV-27"
manifest["processProof"] = {
    "syscalls": {"clone": 220, "exit": 93, "wait4": 260},
    "cloneFlags": 17,
    "cloneArguments": ["SIGCHLD", "NULL", "NULL", "NULL", "NULL"],
    "sharedAddressSpace": False,
    "userNamespace": False,
    "separateExecutable": False,
    "childExitCode": 37,
    "waitStatus": 9472,
}
path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
PY
"$python" "$common_fixture_dir/build_initramfs.py" validate \
    "$output_dir/initramfs.cpio" "$output_dir/init.elf"
"$python" "$fixture_dir/make_platform_dts.py" \
    "$base_dts" "$output_dir/initramfs-manifest.json" \
    "$output_dir/platform-initramfs.dts"
dtc -I dts -O dtb -o "$output_dir/platform-initramfs.dtb" \
    "$output_dir/platform-initramfs.dts"

cat > "$output_dir/build-provenance.txt" <<EOF
phase=RISCV-27
source_assembly=tests/fixtures/riscv_linux_process_lifecycle/init.S
linker_script=tests/fixtures/riscv_linux_process_lifecycle/init.ld
archive_builder=tests/fixtures/riscv_linux_initramfs/build_initramfs.py
compiler=$($cc --version | head -n 1)
readelf=$($readelf --version | head -n 1)
objdump=$($objdump --version | head -n 1)
python=$($python --version 2>&1)
dtc=$(dtc --version)
compile_command=$cc -march=rv64ima_zicsr_zifencei -mabi=lp64 -mno-relax -mno-riscv-attribute -c
link_command=$cc -march=rv64ima_zicsr_zifencei -mabi=lp64 -nostdlib -nostartfiles -static -T init.ld -z max-page-size=4096 --no-relax --build-id=none
EOF
(cd "$output_dir" && sha256sum init.elf initramfs.cpio platform-initramfs.dtb > sha256.txt)
echo "Artifacts: $output_dir"
