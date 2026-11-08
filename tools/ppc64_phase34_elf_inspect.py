#!/usr/bin/env python3
"""Inspect the authenticated PPC64 EXTSW symbol and its call site."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


EXPECTED_KERNEL_SHA256 = (
    "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd"
)
EXTSW_NIP = 0xC00000000138A414


def c_string(data: bytes, offset: int) -> str:
    end = data.find(b"\0", offset)
    if end < 0:
        raise ValueError("unterminated ELF string")
    return data[offset:end].decode("utf-8", errors="replace")


def sign_extend(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


def disassemble(word: int, address: int) -> str:
    opcode = (word >> 26) & 0x3F
    rt = (word >> 21) & 31
    ra = (word >> 16) & 31
    rb = (word >> 11) & 31
    xo = (word >> 1) & 0x3FF
    rc = word & 1
    if opcode == 31 and xo == 986 and rb == 0:
        return f"extsw{' .' if rc else ''} r{ra},r{rt}"
    if opcode == 31 and xo == 444:
        return f"or{' .' if rc else ''} r{ra},r{rt},r{rb}"
    if opcode == 31 and xo == 40:
        return f"subf{' .' if rc else ''} r{rt},r{ra},r{rb}"
    if opcode == 31 and xo == 27:
        return f"sld{' .' if rc else ''} r{ra},r{rt},r{rb}"
    if opcode == 31 and xo == 539:
        return f"srd{' .' if rc else ''} r{ra},r{rt},r{rb}"
    if opcode == 31 and xo == 467:
        return f"mtspr/XFX raw=0x{word:08x}"
    if opcode == 31 and xo == 339:
        return f"mfspr/XFX raw=0x{word:08x}"
    if opcode == 14:
        return f"addi r{rt},r{ra},{sign_extend(word & 0xffff, 16)}"
    if opcode == 18:
        disp = sign_extend(word & 0x03fffffc, 26)
        target = disp if (word & 2) else (address + disp) & 0xFFFFFFFFFFFFFFFF
        return f"b{'l' if word & 1 else ''} 0x{target:016x}"
    if opcode == 16:
        disp = sign_extend(word & 0xfffc, 16)
        target = disp if (word & 2) else (address + disp) & 0xFFFFFFFFFFFFFFFF
        return f"bc bo={(word >> 21) & 31},bi={(word >> 16) & 31},0x{target:016x}"
    if opcode == 10:
        return f"cmplwi/cmpi raw=0x{word:08x}"
    if opcode in (32, 33, 34, 35, 40, 42):
        return f"load/D-form raw=0x{word:08x}"
    if opcode in (36, 37, 44, 45):
        return f"store/D-form raw=0x{word:08x}"
    if opcode == 58:
        return f"ld/ldu/lwa raw=0x{word:08x}"
    if opcode == 62:
        return f"std/stdu raw=0x{word:08x}"
    if opcode == 30:
        md_xo = (word >> 2) & 0x7
        rs = (word >> 21) & 31
        ra = (word >> 16) & 31
        return (f"opcode30 MD_XO={md_xo} RS={rs} RA={ra} "
                f"Rc={rc} (outside supported MD forms)")
    return f".long 0x{word:08x}"


def inspect(kernel: Path, frontier: int | None) -> str:
    data = kernel.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != EXPECTED_KERNEL_SHA256:
        raise ValueError(f"unexpected kernel SHA-256: {digest}")
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4:6] != b"\x02\x02":
        raise ValueError("expected ELF64 big-endian PPC64 kernel")
    header = struct.unpack_from(">16sHHIQQQIHHHHHH", data, 0)
    _, elf_type, machine, _, entry, phoff, shoff, _, _, phentsize, phnum, \
        shentsize, shnum, _ = header
    if elf_type != 2 or machine != 21:
        raise ValueError("expected EM_PPC64 ET_EXEC kernel")
    sections = [struct.unpack_from(">IIQQQQIIQQ", data, shoff + i * shentsize)
                for i in range(shnum)]
    string_tables = {i: data[s[4]:s[4] + s[5]] for i, s in enumerate(sections)
                     if s[1] == 3}
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

    def one_symbol(name: str) -> tuple[str, int, int, int]:
        matches = [symbol for symbol in symbols if symbol[0] == name]
        if len(matches) != 1:
            raise ValueError(f"expected one {name} symbol, found {len(matches)}")
        return matches[0]

    target = one_symbol("fdt_path_offset")
    loads = [struct.unpack_from(">IIQQQQQQ", data, phoff + i * phentsize)
             for i in range(phnum)]
    loads = [program for program in loads if program[0] == 1]

    def virtual_offset(address: int, width: int = 4) -> int:
        for _, _, file_offset, virtual, _, file_size, _, _ in loads:
            if virtual <= address and address + width <= virtual + file_size:
                return file_offset + address - virtual
        raise ValueError(f"address 0x{address:x} is not file-backed")

    def word_at(address: int) -> int:
        return struct.unpack_from(">I", data, virtual_offset(address))[0]

    def branch_target(pc: int) -> int:
        word = word_at(pc)
        if (word >> 26) != 18:
            raise ValueError(f"instruction at 0x{pc:x} is not an I-form branch")
        displacement = sign_extend(word & 0x03fffffc, 26)
        return (displacement if (word & 2) else (pc + displacement)) & \
            0xFFFFFFFFFFFFFFFF

    def symbol_at(address: int) -> str:
        matches = [symbol for symbol in symbols if symbol[2] and
                   symbol[1] <= address < symbol[1] + symbol[2]]
        matches.sort(key=lambda symbol: symbol[2])
        return matches[0][0] if matches else "unknown"

    address = frontier if frontier is not None else EXTSW_NIP
    containing = [symbol for symbol in symbols
                  if symbol[2] and symbol[1] <= address < symbol[1] + symbol[2]]
    caller = [symbol for symbol in symbols
              if symbol[2] and symbol[1] <= 0xC000000001F22568 <
              symbol[1] + symbol[2]]
    if len(containing) > 1:
        containing.sort(key=lambda symbol: symbol[2])
    code_start = target[1]
    code_end = target[1] + target[2]
    lines = [
        "result=PASS",
        f"kernel_sha256={digest}",
        "elf=ELF64 big-endian EM_PPC64 ET_EXEC",
        f"entry=0x{entry:016x}",
        f"fdt_path_offset_symbol=0x{target[1]:016x}",
        f"fdt_path_offset_size=0x{target[2]:x}",
        f"extsw_nip=0x{EXTSW_NIP:016x}",
        f"extsw_symbol_offset=0x{EXTSW_NIP - target[1]:x}",
        f"extsw_word=0x{word_at(EXTSW_NIP):08x}",
        f"attribution_address=0x{address:016x}",
        "attribution_symbol=" + (containing[0][0] if containing else "unknown"),
        "attribution_symbol_offset=" + (f"0x{address - containing[0][1]:x}"
                                         if containing else "unknown"),
        "attribution_symbol_address=" +
            (f"0x{containing[0][1]:016x}" if containing else "unknown"),
        "attribution_symbol_size=" +
            (f"0x{containing[0][2]:x}" if containing else "unknown"),
        "caller_symbol=" + (caller[0][0] if caller else "unknown"),
        "caller_symbol_address=" + (f"0x{caller[0][1]:016x}" if caller else "unknown"),
        "caller_callsite=0xc000000001f22568",
        f"caller_callsite_word=0x{word_at(0xC000000001F22568):08x}",
        "caller_source_location=drivers/of/fdt.c:early_init_dt_scan_root",
        'caller_source_expression=fdt_path_offset(initial_boot_params, "/")',
        f"length_helper_callsite=0x{target[1] + 0x24:016x}",
        f"length_helper_target=0x{branch_target(target[1] + 0x24):016x}",
        f"length_helper_symbol={symbol_at(branch_target(target[1] + 0x24))}",
        f"post_extsw_tail_branch=0x{target[1] + 0x4c:016x}",
        f"post_extsw_tail_target=0x{branch_target(target[1] + 0x4c):016x}",
        f"post_extsw_tail_symbol={symbol_at(branch_target(target[1] + 0x4c))}",
        "fdt_path_offset_instruction_words=",
    ]
    for pc in range(code_start, code_end, 4):
        word = word_at(pc)
        lines.append(f"  0x{pc:016x}: 0x{word:08x}  {disassemble(word, pc)}")

    if frontier is not None:
        frontier_word = word_at(frontier)
        lines.append(f"frontier_word=0x{frontier_word:08x}")
        lines.append("frontier_independent_decode=" +
                     disassemble(frontier_word, frontier))
        lines.append("frontier_static_context_begin")
        for pc in range(frontier - 0x40, frontier + 0x44, 4):
            word = word_at(pc)
            lines.append(f"  0x{pc:016x}: 0x{word:08x}  {disassemble(word, pc)}")
        lines.append("frontier_static_context_end")

    # Find the real caller branch to this function's ELFv2 local entry.
    local_entry = target[1] + 8
    call_sites: list[tuple[int, int]] = []
    for _, _, _, virtual, _, file_size, _, _ in loads:
        start = (virtual + 3) & ~3
        end = virtual + file_size
        for pc in range(start, end - 3, 4):
            word = word_at(pc)
            if (word >> 26) != 18 or (word & 1) == 0:
                continue
            disp = sign_extend(word & 0x03fffffc, 26)
            branch_target = disp if (word & 2) else (pc + disp) & 0xFFFFFFFFFFFFFFFF
            if branch_target == local_entry:
                call_sites.append((pc, word))
    lines.append(f"calls_to_fdt_path_offset_local_entry={len(call_sites)}")
    for pc, word in call_sites[:8]:
        containing_caller = [symbol for symbol in symbols
                             if symbol[2] and symbol[1] <= pc <
                             symbol[1] + symbol[2]]
        containing_caller.sort(key=lambda symbol: symbol[2])
        caller_name = containing_caller[0][0] if containing_caller else "unknown"
        lines.append(f"callsite=0x{pc:016x} caller={caller_name} "
                     f"word=0x{word:08x} target=0x{local_entry:016x}")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--address", type=lambda value: int(value, 0))
    args = parser.parse_args()
    report = inspect(args.kernel, args.address)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report, encoding="utf-8")
    print(report, end="")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        raise SystemExit(f"PPC64 Phase 34 ELF inspection failed: {error}")
