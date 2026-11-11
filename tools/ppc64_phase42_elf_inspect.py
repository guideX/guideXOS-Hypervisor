#!/usr/bin/env python3
"""Independently inspect the authenticated Linux FSCR instruction in ELF."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


EXPECTED_KERNEL_SHA256 = (
    "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd"
)
FRONTIER = 0xC00000000003AD04


def c_string(data: bytes, offset: int) -> str:
    end = data.find(b"\0", offset)
    if end < 0:
        raise ValueError("unterminated ELF string")
    return data[offset:end].decode("utf-8", errors="replace")


def sign_extend(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


def branch_target(address: int, word: int) -> int | None:
    opcode = word >> 26
    if opcode == 18:
        displacement = sign_extend((word >> 2) & 0xFFFFFF, 24) << 2
        absolute = bool(word & 2)
        return (displacement if absolute else address + displacement) & 0xFFFFFFFFFFFFFFFF
    if opcode == 16:
        displacement = sign_extend((word >> 2) & 0x3FFF, 14) << 2
        absolute = bool(word & 2)
        return (displacement if absolute else address + displacement) & 0xFFFFFFFFFFFFFFFF
    return None


def inspect(kernel: Path) -> str:
    data = kernel.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != EXPECTED_KERNEL_SHA256:
        raise ValueError(f"unexpected kernel SHA-256: {digest}")
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4:6] != b"\x02\x02":
        raise ValueError("expected ELF64 big-endian image")

    header = struct.unpack_from(">16sHHIQQQIHHHHHH", data, 0)
    _, elf_type, machine, _, entry, phoff, shoff, _, _, phentsize, phnum, \
        shentsize, shnum, _ = header
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
    symbols: list[tuple[str, int, int, int, int]] = []
    for section in sections:
        if section[1] != 2:
            continue
        strings = string_tables[section[6]]
        entry_size = section[9] or 24
        for offset in range(section[4], section[4] + section[5], entry_size):
            name, info, _, shndx, value, size = struct.unpack_from(
                ">IBBHQQ", data, offset)
            if name:
                symbols.append((c_string(strings, name), value, size, shndx,
                                info & 0xF))

    loads = []
    for index in range(phnum):
        ph = struct.unpack_from(">IIQQQQQQ", data, phoff + index * phentsize)
        if ph[0] == 1:
            loads.append(ph)

    def file_offset(address: int, width: int = 4) -> int:
        for _, _, offset, virtual, _, file_size, _, _ in loads:
            if virtual <= address and address + width <= virtual + file_size:
                return offset + address - virtual
        raise ValueError(f"address {address:#x} is not file-backed")

    def word_at(address: int) -> int:
        return struct.unpack_from(">I", data, file_offset(address))[0]

    word = word_at(FRONTIER)
    primary = (word >> 26) & 0x3F
    rt = (word >> 21) & 0x1F
    low = (word >> 16) & 0x1F
    high = (word >> 11) & 0x1F
    spr = low | (high << 5)
    xo = (word >> 1) & 0x3FF
    if (word, primary, rt, low, high, spr, xo, word & 1) != \
            (0x7D3922A6, 31, 9, 25, 4, 153, 339, 0):
        raise ValueError(f"unexpected frontier instruction {word:#010x}")

    function_symbols = [item for item in symbols if item[4] == 2 and item[2] > 0]
    containing = [item for item in function_symbols
                  if item[1] <= FRONTIER < item[1] + item[2]]
    containing.sort(key=lambda item: (item[1], item[2]))
    nearest = [item for item in function_symbols if item[1] <= FRONTIER]
    nearest.sort(key=lambda item: item[1])
    owner = containing[-1] if containing else (nearest[-1] if nearest else None)

    lines = [
        "result=PASS",
        f"kernel={kernel}",
        f"kernel_sha256={digest}",
        "elf=ELF64 big-endian EM_PPC64 ET_EXEC",
        f"entry=0x{entry:016x}",
        f"frontier_nip=0x{FRONTIER:016x}",
        f"frontier_word=0x{word:08x}",
        f"primary={primary}",
        f"RT=r{rt}",
        f"spr_low5={low}",
        f"spr_high5={high}",
        f"spr=({high}<<5)|{low}={spr}",
        f"XO={xo}",
        f"reserved_final_bit={word & 1}",
        f"frontier_file_offset=0x{file_offset(FRONTIER):x}",
    ]
    if owner is None:
        lines.extend(["containing_symbol=none", "symbol_offset=unavailable"])
    else:
        name, value, size, _, _ = owner
        lines.extend([
            f"containing_symbol={name}" if value <= FRONTIER < value + size
            else f"nearest_preceding_symbol={name}",
            f"symbol_address=0x{value:016x}",
            f"symbol_size=0x{size:x}",
            f"symbol_offset=0x{FRONTIER - value:x}",
            f"symbol_contains_frontier={str(value <= FRONTIER < value + size).lower()}",
        ])

    start = FRONTIER - 64
    end = FRONTIER + 68
    lines.append("instruction_window_begin")
    for address in range(start, end, 4):
        current = word_at(address)
        target = branch_target(address, current)
        suffix = f" branch_target=0x{target:016x}" if target is not None else ""
        lines.append(f"0x{address:016x}: 0x{current:08x}{suffix}")
    lines.append("instruction_window_end")
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
        raise SystemExit(f"PPC64 Phase 42 ELF inspection failed: {error}")
