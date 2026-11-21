#!/usr/bin/env python3
"""Create the deterministic RISCV-29 initramfs FDT from the accepted platform."""

import json
import pathlib
import sys


RAM_BASE = 0x80000000
RAM_SIZE = 0x04000000
INITRD_START = 0x81400000
FDT_START = 0x82200000
FDT_SIZE = 0x3000


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: make_platform_dts.py BASE_DTS MANIFEST OUTPUT_DTS")
    source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    manifest = json.loads(pathlib.Path(sys.argv[2]).read_text(encoding="utf-8"))
    archive_size = int(manifest["archiveBytes"])
    initrd_end = INITRD_START + archive_size
    if archive_size <= 0 or initrd_end > RAM_BASE + RAM_SIZE:
        raise ValueError("initramfs range is empty or outside guest RAM")
    if INITRD_START < FDT_START + FDT_SIZE and FDT_START < initrd_end:
        raise ValueError("initramfs range overlaps the FDT")
    old_compatible = 'compatible = "guidexos,riscv18-linux-entry";'
    if source.count(old_compatible) != 1:
        raise ValueError("base DTS is not the accepted pinned Linux platform")
    bootargs = 'bootargs = "console=ttyS0,115200 earlycon=uart8250,mmio,0x10000000";'
    if source.count(bootargs) != 1:
        raise ValueError("base DTS has an unexpected bootargs profile")
    soc_header = "\tsoc {\n\t\t#address-cells = <2>;"
    if source.count(soc_header) != 1:
        raise ValueError("base DTS does not contain the accepted SoC bus")
    source = source.replace("guideXOS RISCV-18 Linux entry profile",
                            "guideXOS RISCV-29 repeated process profile")
    source = source.replace(old_compatible,
                            'compatible = "guidexos,riscv29-linux-process-repeat";')
    source = source.replace(soc_header,
        '\tsoc {\n\t\tcompatible = "simple-bus";\n\t\t#address-cells = <2>;')
    properties = (bootargs + "\n\t\tlinux,initrd-start = <0x0 0x%08x>;\n"
                  "\t\tlinux,initrd-end = <0x0 0x%08x>;" %
                  (INITRD_START, initrd_end))
    source = source.replace(bootargs, properties)
    pathlib.Path(sys.argv[3]).write_text(source, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
