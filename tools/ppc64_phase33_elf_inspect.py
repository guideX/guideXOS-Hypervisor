#!/usr/bin/env python3
"""Independently locate Linux setup_paca's SPRG1 install in its ELF image."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


EXPECTED_KERNEL_SHA256 = (
    "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd"
)


def c_string(data: bytes, offset: int) -> str:
    end = data.find(b"\0", offset)
    if end < 0:
        raise ValueError("unterminated ELF string")
    return data[offset:end].decode("utf-8", errors="replace")


def decode_spr(word: int) -> tuple[int, int, int, int]:
    """Decode the XFX fields, reversing the architectural SPR field halves."""
    primary = (word >> 26) & 0x3F
    source = (word >> 21) & 0x1F
    low = (word >> 16) & 0x1F
    high = (word >> 11) & 0x1F
    spr = (high << 5) | low
    xo = (word >> 1) & 0x3FF
    return primary, source, spr, xo


def inspect(kernel: Path) -> str:
    data = kernel.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != EXPECTED_KERNEL_SHA256:
        raise ValueError(f"unexpected kernel SHA-256: {digest}")
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise ValueError("input is not an ELF image")
    if data[4] != 2 or data[5] != 2:
        raise ValueError("expected ELF64 big-endian input")

    header = struct.unpack_from(">16sHHIQQQIHHHHHH", data, 0)
    _, elf_type, machine, _, entry, phoff, shoff, _, _, phentsize, phnum, \
        shentsize, shnum, shstrndx = header
    if elf_type != 2 or machine != 21:
        raise ValueError("expected PPC64 ET_EXEC kernel")
    sections = [
        struct.unpack_from(">IIQQQQIIQQ", data, shoff + i * shentsize)
        for i in range(shnum)
    ]
    string_tables = {
        index: data[section[4]:section[4] + section[5]]
        for index, section in enumerate(sections) if section[1] == 3
    }
    symbols: list[tuple[str, int, int, int]] = []
    for section in sections:
        if section[1] != 2:
            continue
        strings = string_tables[section[6]]
        entry_size = section[9] or 24
        for offset in range(section[4], section[4] + section[5], entry_size):
            name, _, _, shndx, value, size = struct.unpack_from(
                ">IBBHQQ", data, offset)
            if name:
                symbols.append((c_string(strings, name), value, size, shndx))

    def symbol(name: str) -> tuple[str, int, int, int]:
        matches = [candidate for candidate in symbols if candidate[0] == name]
        if len(matches) != 1:
            raise ValueError(f"expected one {name} symbol, found {len(matches)}")
        return matches[0]

    setup = symbol("setup_paca")
    boot_matches = [candidate for candidate in symbols
                    if candidate[0].startswith("boot_paca") and candidate[2] != 0]
    if len(boot_matches) != 1:
        raise ValueError(f"expected one boot_paca object, found {len(boot_matches)}")
    boot_paca = boot_matches[0]

    loads = []
    for index in range(phnum):
        ph = struct.unpack_from(">IIQQQQQQ", data, phoff + index * phentsize)
        if ph[0] == 1:
            loads.append(ph)

    def virtual_offset(address: int, width: int = 4) -> int:
        for _, _, file_offset, virtual, _, file_size, _, _ in loads:
            if virtual <= address and address + width <= virtual + file_size:
                return file_offset + address - virtual
        raise ValueError(f"address {address:#x} is not file-backed")

    def word_at(address: int) -> int:
        return struct.unpack_from(">I", data, virtual_offset(address))[0]

    if setup[2] < 4 or setup[2] % 4:
        raise ValueError("setup_paca symbol has an invalid instruction extent")
    mtspr_sites = []
    instructions = []
    for offset in range(0, setup[2], 4):
        address = setup[1] + offset
        word = word_at(address)
        instructions.append((offset, word))
        primary, source, spr, xo = decode_spr(word)
        if primary == 31 and source == 3 and spr == 273 and xo == 467 and (word & 1) == 0:
            mtspr_sites.append((offset, address, word))
    if len(mtspr_sites) != 1:
        raise ValueError(f"expected one setup_paca mtspr SPRG1,r3, found {len(mtspr_sites)}")
    mtspr_offset, mtspr_address, mtspr_word = mtspr_sites[0]
    if mtspr_offset < 8:
        raise ValueError("SPRG1 install has no preceding hypervisor-state branch")
    branch_offset = mtspr_offset - 8
    branch_word = word_at(setup[1] + branch_offset)
    if ((branch_word >> 26) & 0x3F) != 16:
        raise ValueError("instruction before the HPACA write is not a conditional branch")
    branch_disp = branch_word & 0xFFFC
    if branch_disp & 0x8000:
        branch_disp -= 0x10000
    branch_target = ((setup[1] + branch_offset + branch_disp)
                     if (branch_word & 2) == 0 else branch_disp) & 0xFFFFFFFFFFFFFFFF
    if branch_target != mtspr_address:
        raise ValueError("HV=0 branch does not skip directly to the SPRG1 install")

    primary, source, spr, xo = decode_spr(mtspr_word)
    lines = [
        "result=PASS",
        f"kernel={kernel}",
        f"kernel_sha256={digest}",
        "elf=ELF64 big-endian EM_PPC64 ET_EXEC",
        f"entry=0x{entry:016x}",
        f"setup_paca_symbol=0x{setup[1]:016x}",
        f"setup_paca_size=0x{setup[2]:x}",
        f"boot_paca_symbol={boot_paca[0]}",
        f"boot_paca_value=0x{boot_paca[1]:016x}",
        f"boot_paca_size=0x{boot_paca[2]:x}",
        f"sprg1_install_offset=0x{mtspr_offset:x}",
        f"sprg1_install_address=0x{mtspr_address:016x}",
        f"sprg1_install_word=0x{mtspr_word:08x}",
        f"sprg1_install_primary={primary}",
        f"sprg1_install_rs=r{source}",
        f"sprg1_install_spr={spr}",
        f"sprg1_install_xo={xo}",
        f"hv_state_branch_offset=0x{branch_offset:x}",
        f"hv_state_branch_word=0x{branch_word:08x}",
        f"hv0_branch_target=0x{branch_target:016x}",
        "setup_paca_instruction_words=" + ",".join(
            f"+0x{offset:x}:0x{word:08x}" for offset, word in instructions),
    ]
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = inspect(args.kernel)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report, encoding="utf-8")
    print(report, end="")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        raise SystemExit(f"PPC64 Phase 33 ELF inspection failed: {error}")
