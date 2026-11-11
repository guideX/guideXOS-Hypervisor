#!/usr/bin/env python3
"""Independently validate an ACPI firmware capture taken from guest RAM."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ACPI20_GUID = bytes.fromhex("71e86888f1e4d311bc220080c73c8881")
EXPECTED_AML = bytes.fromhex("084f454d300d677569646558204941363400")


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def checksum_ok(data: bytes) -> bool:
    return sum(data) & 0xFF == 0


def table_at(memory: bytes, memory_base: int, address: int) -> bytes:
    offset = address - memory_base
    require(offset >= 0 and offset + 36 <= len(memory),
            f"table header at {address:#x} is outside the captured pages")
    length = u32(memory, offset + 4)
    require(length >= 36 and offset + length <= len(memory),
            f"invalid table length {length} at {address:#x}")
    return memory[offset:offset + length]


def validate_snapshot(config_path: Path,
                      pages_path: Path,
                      globals_path: Path,
                      descriptors_path: Path,
                      pages_base: int,
                      globals_base: int,
                      facs_pointer_required: bool) -> list[str]:
    config = config_path.read_bytes()
    pages = pages_path.read_bytes()
    globals_data = globals_path.read_bytes()
    descriptors = descriptors_path.read_bytes()

    require(len(config) == 48, f"EFI configuration capture is {len(config)} bytes, expected 48")
    require(len(pages) >= 0x2000, f"ACPI page capture is {len(pages)} bytes, expected at least 8192")
    require(len(globals_data) >= 24, f"ACPICA globals capture is {len(globals_data)} bytes, expected at least 24")
    require(len(descriptors) >= 64, f"descriptor capture is {len(descriptors)} bytes, expected at least 64")

    # Two 24-byte EFI_CONFIGURATION_TABLE records are present. The second is
    # the ACPI 2.0 entry (GUID followed by its 64-bit physical table pointer).
    acpi_guid = config[24:40]
    require(acpi_guid == ACPI20_GUID, f"ACPI 2.0 EFI GUID mismatch: {acpi_guid.hex()}")
    rsdp_address = u64(config, 40)
    rsdp_offset = rsdp_address - pages_base
    require(0 <= rsdp_offset and rsdp_offset + 36 <= len(pages),
            f"EFI ACPI 2.0 pointer {rsdp_address:#x} is outside captured ACPI pages")

    rsdp = pages[rsdp_offset:rsdp_offset + 36]
    require(rsdp[:8] == b"RSD PTR ", "RSDP signature mismatch")
    require(checksum_ok(rsdp[:20]), "RSDP legacy checksum mismatch")
    require(rsdp[15] == 2 and u32(rsdp, 20) == 36, "RSDP is not a complete ACPI 2.0 record")
    require(checksum_ok(rsdp), "RSDP extended checksum mismatch")
    rsdt_address = u32(rsdp, 16)
    xsdt_address = u64(rsdp, 24)

    rsdt = table_at(pages, pages_base, rsdt_address)
    xsdt = table_at(pages, pages_base, xsdt_address)
    for name, table in (("RSDT", rsdt), ("XSDT", xsdt)):
        require(table[:4] == name.encode("ascii"), f"{name} signature mismatch")
        require(checksum_ok(table), f"{name} checksum mismatch")
    require(len(rsdt) == 44 and len(xsdt) == 52, "root table sizes do not match two entries")
    rsdt_entries = [u32(rsdt, 36 + 4 * i) for i in range(2)]
    xsdt_entries = [u64(xsdt, 36 + 8 * i) for i in range(2)]
    require(xsdt_entries == rsdt_entries, "RSDT and XSDT entries differ")
    madt_address, fadt_address = rsdt_entries

    madt = table_at(pages, pages_base, madt_address)
    fadt = table_at(pages, pages_base, fadt_address)
    require(madt[:4] == b"APIC" and len(madt) == 60 and checksum_ok(madt),
            "MADT signature, size, or checksum mismatch")
    require(fadt[:4] == b"FACP" and len(fadt) == 244 and fadt[8] == 3 and checksum_ok(fadt),
            "FADT v3 signature, size, revision, or checksum mismatch")

    facs_address = u32(fadt, 36)
    dsdt_address = u32(fadt, 40)
    x_facs_address = u64(fadt, 132)
    x_dsdt_address = u64(fadt, 140)
    require(facs_address == x_facs_address, "legacy and extended FACS pointers disagree")
    require(dsdt_address == x_dsdt_address, "legacy and extended DSDT pointers disagree")
    require(facs_address <= 0xFFFFFFFF and dsdt_address <= 0xFFFFFFFF,
            "legacy table pointer exceeds 32-bit address width")

    dsdt = table_at(pages, pages_base, dsdt_address)
    require(dsdt[:4] == b"DSDT" and len(dsdt) == 54 and checksum_ok(dsdt),
            "DSDT signature, size, or checksum mismatch")
    aml = dsdt[36:]
    require(aml == EXPECTED_AML, f"minimal DSDT AML mismatch: {aml.hex()}")

    reclaim_end = pages_base + 0x1000
    reclaim_ranges = [
        (rsdp_address, 36, "RSDP"),
        (rsdt_address, len(rsdt), "RSDT"),
        (xsdt_address, len(xsdt), "XSDT"),
        (madt_address, len(madt), "MADT"),
        (fadt_address, len(fadt), "FADT"),
        (dsdt_address, len(dsdt), "DSDT"),
    ]
    for address, length, name in reclaim_ranges:
        require(address >= pages_base and address + length <= reclaim_end,
                f"{name} is not wholly inside ACPI reclaim memory")
    ordered_ranges = sorted(reclaim_ranges)
    for previous, current in zip(ordered_ranges, ordered_ranges[1:]):
        require(previous[0] + previous[1] <= current[0],
                f"{previous[2]} overlaps {current[2]}")

    # FACS is intentionally validated as an ACPI control structure, not as an
    # ACPI table: it has no standard header checksum.
    facs_offset = facs_address - pages_base
    require(0 <= facs_offset and facs_offset + 64 <= len(pages),
            "FACS is outside captured ACPI pages")
    require(pages_base + 0x1000 <= facs_address and
            facs_address + 64 <= pages_base + 0x2000,
            "FACS is not wholly inside ACPI NVS memory")
    facs = pages[facs_offset:facs_offset + 64]
    require(facs[:4] == b"FACS" and u32(facs, 4) == 64, "FACS signature or length mismatch")
    require(u32(facs, 16) == 0 and u32(facs, 20) == 0,
            "minimal FACS global-lock or flags field is nonzero")
    require(u64(facs, 24) == 0 and facs[32] == 1 and facs[33:36] == b"\0\0\0",
            "minimal FACS extended vector, version, or reserved bytes mismatch")

    # ACPICA's root descriptor array stores the DSDT at index 0 and FACS at
    # index 1 for this table-loading path. Address/length/signature and the
    # low three origin bits are read directly from the captured guest bytes.
    facs_descriptor = descriptors[32:64]
    descriptor_address = u64(facs_descriptor, 0)
    descriptor_pointer = u64(facs_descriptor, 8)
    descriptor_length = u32(facs_descriptor, 16)
    descriptor_signature = facs_descriptor[20:24]
    descriptor_origin_flags = facs_descriptor[25]
    require(descriptor_address == facs_address, "ACPICA FACS descriptor address mismatch")
    require(descriptor_length == 64 and descriptor_signature == b"FACS",
            "ACPICA FACS descriptor length or signature mismatch")
    require(descriptor_origin_flags & 7 == 1,
            f"ACPICA FACS descriptor is not marked mapped (origin flags {descriptor_origin_flags:#x})")

    # The snapshot begins at the DSDT global; the following word is FACS.
    dsdt_global_offset = 0x4CBF058 - globals_base
    facs_global_offset = 0x4CBF060 - globals_base
    require(dsdt_global_offset >= 0 and facs_global_offset >= 0 and
            facs_global_offset + 8 <= len(globals_data),
            "ACPICA global addresses are outside the captured range")
    dsdt_global = u64(globals_data, dsdt_global_offset)
    facs_global = u64(globals_data, facs_global_offset)
    require(dsdt_global != 0, "ACPICA DSDT global pointer is null")
    if facs_pointer_required:
        require(descriptor_pointer != 0, "ACPICA FACS descriptor pointer is still null")
        require(facs_global != 0, "ACPICA global FACS pointer is still null")
        require(facs_global == descriptor_pointer, "ACPICA global and descriptor FACS pointers differ")
    elif descriptor_pointer != 0 or facs_global != 0:
        require(descriptor_pointer == facs_global,
                "ACPICA global and descriptor FACS pointers differ")

    return [
        f"EFI ACPI 2.0 config table: GUID={acpi_guid.hex()} RSDP={rsdp_address:#x}",
        f"RSDP: revision=2 length=36 RSDT={rsdt_address:#x} XSDT={xsdt_address:#x} checksums=PASS",
        f"RSDT: {rsdt_address:#x} length={len(rsdt)} entries={[hex(x) for x in rsdt_entries]} checksum=PASS",
        f"XSDT: {xsdt_address:#x} length={len(xsdt)} entries={[hex(x) for x in xsdt_entries]} checksum=PASS",
        f"MADT: {madt_address:#x} length={len(madt)} checksum=PASS",
        f"FADT: {fadt_address:#x} revision=3 length={len(fadt)} checksum=PASS FACS={facs_address:#x} DSDT={dsdt_address:#x}",
        f"DSDT: {dsdt_address:#x} length={len(dsdt)} AML={aml.hex()} checksum=PASS",
        f"FACS: {facs_address:#x} length=64 version={facs[32]} no_checksum=EXPECTED global_lock={u32(facs,16):#x} flags={u32(facs,20):#x}",
        f"ACPICA FACS descriptor index=1 address={descriptor_address:#x} length={descriptor_length} origin_flags={descriptor_origin_flags:#x} pointer={descriptor_pointer:#x}",
        f"ACPICA globals: DSDT={dsdt_global:#x} FACS={facs_global:#x}",
        f"Physical ranges: ACPI reclaim=[{pages_base:#x},{pages_base+0x1000:#x}) NVS=[{pages_base+0x1000:#x},{pages_base+0x2000:#x})",
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True, help="48-byte EFI configuration-table capture")
    parser.add_argument("--pages", type=Path, required=True, help="two-page ACPI capture (reclaim page then NVS page)")
    parser.add_argument("--globals", type=Path, required=True, help="ACPICA DSDT/FACS global pointer capture")
    parser.add_argument("--descriptors", type=Path, required=True, help="ACPICA root table descriptor array capture")
    parser.add_argument("--pages-base", type=lambda v: int(v, 0), default=0x1FDB3000)
    parser.add_argument("--globals-base", type=lambda v: int(v, 0), default=0x4CBF050)
    parser.add_argument("--require-facs-pointer", action="store_true",
                        help="require ACPICA to have materialized the FACS mapping")
    args = parser.parse_args()
    try:
        for line in validate_snapshot(args.config, args.pages, args.globals,
                                      args.descriptors, args.pages_base,
                                      args.globals_base, args.require_facs_pointer):
            print(line)
    except (OSError, ValueError, struct.error) as exc:
        parser.error(str(exc))
    print("Independent guest-RAM ACPI validation: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
