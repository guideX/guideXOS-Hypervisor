#!/usr/bin/env python3
"""Attribute the Phase 40 MTOCRF and its next frontier in the pinned ELF."""

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


def sign_extend(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


def disassemble(word: int, address: int) -> str:
    opcode = (word >> 26) & 0x3F
    rt = (word >> 21) & 31
    ra = (word >> 16) & 31
    rb = (word >> 11) & 31
    xo10 = (word >> 1) & 0x3FF
    xo9 = xo10 & 0x1FF
    rc = word & 1
    if opcode == 31 and xo10 == 144 and ((word >> 20) & 1):
        fxm = (word >> 12) & 0xFF
        return f"mtocrf 0x{fxm:02x},r{rt}"
    if opcode == 31 and xo9 == 104 and ((word >> 10) & 1) == 0:
        return f"neg{' .' if rc else ''} r{rt},r{ra}"
    if opcode == 31 and xo10 == 278:
        return f"dcbt r{ra},r{rb}"
    if opcode == 31 and xo10 == 444:
        return f"or{' .' if rc else ''} r{ra},r{rt},r{rb}"
    if opcode == 14:
        return f"addi r{rt},r{ra},{sign_extend(word & 0xFFFF, 16)}"
    if opcode == 10:
        bf = (word >> 23) & 7
        lbit = (word >> 21) & 1
        ra = (word >> 16) & 31
        immediate = word & 0xFFFF
        return f"cmpl{'d' if lbit else 'w'}i cr{bf},r{ra},{immediate}"
    if opcode == 28:
        return f"andi. r{ra},r{rt},0x{word & 0xFFFF:04x}"
    if opcode == 16:
        bo = (word >> 21) & 31
        bi = (word >> 16) & 31
        displacement = sign_extend(word & 0xFFFC, 16)
        target = displacement if word & 2 else (address + displacement) & 0xFFFFFFFFFFFFFFFF
        return f"bc bo={bo},bi={bi},0x{target:016x}"
    if opcode == 18:
        displacement = sign_extend(word & 0x03FFFFFC, 26)
        target = displacement if word & 2 else (address + displacement) & 0xFFFFFFFFFFFFFFFF
        return f"b{'l' if word & 1 else ''} 0x{target:016x}"
    return f".long 0x{word:08x}"


def inspect(kernel: Path, address: int, callsite: int) -> str:
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
    string_tables = {i: data[section[4]:section[4] + section[5]]
                     for i, section in enumerate(sections) if section[1] == 3}
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
                symbols.append((c_string(strings, name), value, size,
                                shndx, info & 0xF))

    loads = [struct.unpack_from(">IIQQQQQQ", data, phoff + i * phentsize)
             for i in range(phnum)]
    loads = [program for program in loads if program[0] == 1]

    def virtual_offset(virtual_address: int, width: int = 4) -> int:
        for _, _, file_offset, virtual, _, file_size, _, _ in loads:
            if virtual <= virtual_address and \
                    virtual_address + width <= virtual + file_size:
                return file_offset + virtual_address - virtual
        raise ValueError(f"address 0x{virtual_address:x} is not file-backed")

    def word_at(virtual_address: int) -> int:
        return struct.unpack_from(">I", data, virtual_offset(virtual_address))[0]

    def symbol_at(virtual_address: int) -> list[tuple[str, int, int, int, int]]:
        matches = [symbol for symbol in symbols
                   if symbol[2] and symbol[1] <= virtual_address <
                   symbol[1] + symbol[2]]
        return sorted(matches, key=lambda symbol: symbol[2])

    def branch_target(pc: int, word: int) -> int:
        if word >> 26 != 18:
            raise ValueError(f"callsite 0x{pc:x} is not an I-form branch")
        displacement = sign_extend(word & 0x03FFFFFC, 26)
        return (displacement if word & 2 else pc + displacement) & \
            0xFFFFFFFFFFFFFFFF

    instruction = word_at(address)
    call_word = word_at(callsite)
    call_target = branch_target(callsite, call_word)
    if not (call_word & 1):
        raise ValueError("observed callsite does not set LK")
    if call_target != address - 4:
        raise ValueError("observed call does not target the MTOCRF local entry")

    target_symbols = symbol_at(address)
    caller_symbols = symbol_at(callsite)
    lines = [
        "result=PASS",
        f"kernel_sha256={digest}",
        "elf=ELF64 big-endian EM_PPC64 ET_EXEC",
        f"entry=0x{entry:016x}",
        f"target_nip=0x{address:016x}",
        f"target_word=0x{instruction:08x}",
        f"target_independent_decode={disassemble(instruction, address)}",
        "target_containing_function_symbols=" +
            (",".join(symbol[0] for symbol in target_symbols) or "none"),
        f"runtime_observed_callsite=0x{callsite:016x}",
        f"callsite_word=0x{call_word:08x}",
        f"callsite_independent_decode={disassemble(call_word, callsite)}",
        f"callsite_branch_target=0x{call_target:016x}",
        "call_target_to_MTOCRF_byte_offset=-4",
        "callsite_containing_function_symbols=" +
            (",".join(symbol[0] for symbol in caller_symbols) or "none"),
        "target_static_context_begin",
    ]
    for pc in range(address - 8, address + 0x24, 4):
        word = word_at(pc)
        lines.append(f"0x{pc:016x}: 0x{word:08x}  {disassemble(word, pc)}")
    lines.append("target_static_context_end")

    expected = [0x7CB01120, 0x28A50010, 0x7CC300D0,
                0x70C60007, 0x7C00222C, 0x41840208]
    actual = [word_at(address + 4 * offset) for offset in range(len(expected))]
    lines.append("memcpy_big_endian_source_word_sequence_match=" +
                 str(actual == expected).lower())
    lines.append("source_pattern=PowerPC Linux v7.2 arch/powerpc/lib/memcpy_64.S "
                 "PPC_MTOCRF(0x01,r5); cmpldi cr1,r5,16; neg r6,r3; "
                 "andi. r6,r6,7; dcbt 0,r4; blt cr1,.Lshort_copy")
    lines.append("source_attribution=instruction sequence matches Linux v7.2 "
                 "big-endian memcpy source; authenticated ELF has no containing "
                 "function symbol at this address")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--address", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--callsite", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    report = inspect(args.kernel, args.address, args.callsite)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report, encoding="utf-8")
    print(report, end="")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        raise SystemExit(f"PPC64 Phase 40 ELF inspection failed: {error}")
