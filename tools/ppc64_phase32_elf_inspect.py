#!/usr/bin/env python3
"""Attribute the authenticated Phase 32 SUBFE frontier in its Linux ELF."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


EXPECTED_KERNEL_SHA256 = (
    "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd"
)
SUBFE_NIP = 0xC000000001EAA5BC
ADDIC_NIP = SUBFE_NIP - 4
WARNING_TDI_NIP = SUBFE_NIP + 4
SETUP_PACA_NIP = 0xC000000000034D30


def c_string(data: bytes, offset: int) -> str:
    end = data.find(b"\0", offset)
    if end < 0:
        raise ValueError("unterminated ELF string")
    return data[offset:end].decode("utf-8", errors="replace")


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
    shstr = data[sections[shstrndx][4]:
                 sections[shstrndx][4] + sections[shstrndx][5]]
    section_names = [c_string(shstr, section[0]) if section[0] else ""
                     for section in sections]

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

    def unique_symbol(name: str) -> tuple[str, int, int, int]:
        matches = [symbol for symbol in symbols if symbol[0] == name]
        if len(matches) != 1:
            raise ValueError(f"expected one {name} symbol, found {len(matches)}")
        return matches[0]

    early_setup = unique_symbol("early_setup")
    setup_paca = unique_symbol("setup_paca")
    if not (early_setup[1] <= SUBFE_NIP < early_setup[1] + early_setup[2]):
        raise ValueError("SUBFE frontier is outside early_setup")
    if setup_paca[1] != SETUP_PACA_NIP:
        raise ValueError("setup_paca symbol address differs from expected call target")

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

    addic_word = word_at(ADDIC_NIP)
    subfe_word = word_at(SUBFE_NIP)
    warning_word = word_at(WARNING_TDI_NIP)
    setup_call = word_at(SUBFE_NIP + 12)
    setup_paca_r13_write = word_at(SETUP_PACA_NIP + 4)
    if (addic_word, subfe_word, warning_word) != (
            0x312DFFFF, 0x7D296910, 0x0B090000):
        raise ValueError("authenticated frontier instruction bytes changed")

    # PowerPC B-form opcode 18 uses a signed 26-bit displacement after
    # clearing the low two alignment bits. Decode the call target directly.
    if setup_call >> 26 != 18 or (setup_call & 1) == 0:
        raise ValueError("instruction after the warning is not a linked branch")
    displacement = setup_call & 0x03FFFFFC
    if displacement & 0x02000000:
        displacement -= 0x04000000
    branch_target = (displacement if setup_call & 2 else SUBFE_NIP + 12 +
                     displacement) & 0xFFFFFFFFFFFFFFFF
    if branch_target != SETUP_PACA_NIP:
        raise ValueError("authenticated setup_paca call target did not resolve")
    if setup_paca_r13_write != 0x7C6D1B78:
        raise ValueError("setup_paca does not begin by copying r3 into r13")

    bug_table = next((section for section, name in zip(sections, section_names)
                      if name == "__bug_table"), None)
    if bug_table is None or bug_table[5] % 12:
        raise ValueError("missing or malformed authenticated __bug_table")
    bug_bytes = data[bug_table[4]:bug_table[4] + bug_table[5]]
    bug_match = None
    for index in range(bug_table[5] // 12):
        offset = index * 12
        bug_disp, file_disp, line, flags = struct.unpack_from(
            ">iiHH", bug_bytes, offset)
        address = bug_table[3] + offset + bug_disp
        if address == WARNING_TDI_NIP:
            file_address = bug_table[3] + offset + 4 + file_disp
            file_offset = virtual_offset(file_address, 1)
            bug_match = (c_string(data, file_offset), line, flags)
            break
    if bug_match != ("arch/powerpc/kernel/setup_64.c", 364, 0x0901):
        raise ValueError(f"unexpected authenticated warning-site bug entry: {bug_match}")

    offset = SUBFE_NIP - early_setup[1]
    return "\n".join([
        "result=PASS",
        f"kernel={kernel}",
        f"kernel_sha256={digest}",
        "elf=ELF64 big-endian EM_PPC64 ET_EXEC",
        f"entry=0x{entry:016x}",
        f"enclosing_symbol={early_setup[0]}",
        f"symbol_value=0x{early_setup[1]:016x}",
        f"symbol_size=0x{early_setup[2]:x}",
        f"symbol_offset=0x{offset:x}",
        f"addic=0x{ADDIC_NIP:016x}:0x{addic_word:08x}",
        f"subfe=0x{SUBFE_NIP:016x}:0x{subfe_word:08x}",
        f"warning_tdi=0x{WARNING_TDI_NIP:016x}:0x{warning_word:08x}",
        f"warning_source={bug_match[0]}:{bug_match[1]}",
        f"warning_flags=0x{bug_match[2]:04x}",
        f"setup_paca_symbol=0x{setup_paca[1]:016x}",
        f"linked_branch=0x{SUBFE_NIP + 12:016x}:0x{setup_call:08x}",
        f"linked_branch_target=0x{branch_target:016x}",
        f"setup_paca_r13_copy=0x{SETUP_PACA_NIP + 4:016x}:0x{setup_paca_r13_write:08x}",
        "setup_paca_r13_copy_decode=or r13,r3,r3",
        "source_context=early_setup WARN_ON(local_paca), then setup_paca(&boot_paca)",
    ]) + "\n"


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
        raise SystemExit(f"PPC64 Phase 32 ELF inspection failed: {error}")
