#!/usr/bin/env python3
"""Verify captured EFI_FILE_PROTOCOL.Read bytes against a host source file."""

from __future__ import annotations

import argparse
import csv
import hashlib
from pathlib import Path


def hex_field(row: dict[str, str], name: str) -> int:
    try:
        return int(row[name], 16)
    except (KeyError, ValueError) as error:
        raise ValueError(f"invalid hexadecimal field {name!r}: {row.get(name)!r}") from error


def missing_ranges(covered: bytearray) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    cursor = 0
    while cursor < len(covered):
        if covered[cursor]:
            cursor += 1
            continue
        start = cursor
        while cursor < len(covered) and not covered[cursor]:
            cursor += 1
        ranges.append((start, cursor))
    return ranges


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_base", help="base path used with --capture-efi-file-reads")
    parser.add_argument("source", type=Path, help="authentic host-side file backing the guest path")
    parser.add_argument("--guest-path", default="linux", help="guest-visible file path (default: linux)")
    parser.add_argument(
        "--allow-position-discontinuities",
        action="store_true",
        help="allow cursor jumps between reads (for captures that also use SetPosition)",
    )
    args = parser.parse_args()

    metadata_path = Path(f"{args.capture_base}.tsv")
    payload_path = Path(f"{args.capture_base}.bin")
    source = args.source.read_bytes()
    payload = payload_path.read_bytes()
    source_hash = hashlib.sha256(source).hexdigest().upper()

    with metadata_path.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        required = {
            "handle", "path", "requested", "actual", "position_before", "position_after", "status",
            "guest_buffer_verified", "payload_offset", "payload_size",
        }
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise ValueError(f"capture metadata is missing required columns: {sorted(required)}")
        rows = [row for row in reader if (row.get("path") or "").casefold() == args.guest_path.casefold()]

    covered = bytearray(len(source))
    reconstructed = bytearray(len(source))
    transferred = 0
    overlap_bytes = 0
    successful_records = 0
    last_position_by_handle: dict[str, int] = {}

    for row in rows:
        status = hex_field(row, "status")
        requested = hex_field(row, "requested")
        actual = hex_field(row, "actual")
        offset = hex_field(row, "position_before")
        position_after = hex_field(row, "position_after")
        payload_offset = hex_field(row, "payload_offset")
        payload_size = hex_field(row, "payload_size")
        handle = (row.get("handle") or "").casefold()

        if actual > requested:
            raise ValueError(f"Read returned more bytes than requested: {row}")
        if not args.allow_position_discontinuities and handle in last_position_by_handle:
            expected_position = last_position_by_handle[handle]
            if offset != expected_position:
                raise ValueError(
                    f"file cursor jumped for handle {handle}: expected 0x{expected_position:x}, "
                    f"observed 0x{offset:x} (capture row {row.get('index')})"
                )
        last_position_by_handle[handle] = position_after

        if status != 0:
            if actual != 0 or payload_size != 0:
                raise ValueError(f"failed read reports transferred bytes: {row}")
            if position_after != offset:
                raise ValueError(f"failed read changed file position: {row}")
            continue
        successful_records += 1
        if actual != payload_size:
            raise ValueError(f"captured byte count differs from Read result: {row}")
        if actual == 0:
            if position_after != offset:
                raise ValueError(f"zero-byte read changed file position: {row}")
            continue
        if row.get("guest_buffer_verified") != "1":
            raise ValueError(f"guest destination was not verified: {row}")
        if position_after != offset + actual:
            raise ValueError(f"file position did not advance by the returned byte count: {row}")
        if offset > len(source) or actual > len(source) - offset:
            raise ValueError(f"read extends past the independent source file: {row}")
        if payload_offset > len(payload) or payload_size > len(payload) - payload_offset:
            raise ValueError(f"read payload extends past the capture byte stream: {row}")

        guest_bytes = payload[payload_offset:payload_offset + payload_size]
        source_bytes = source[offset:offset + actual]
        if guest_bytes != source_bytes:
            raise ValueError(
                f"guest bytes differ from source at file offset 0x{offset:x} "
                f"(capture row {row.get('index')})"
            )
        transferred += actual
        for index, value in enumerate(guest_bytes, offset):
            if covered[index]:
                overlap_bytes += 1
                if reconstructed[index] != value:
                    raise ValueError(f"overlapping reads disagree at source offset 0x{index:x}")
            else:
                covered[index] = 1
                reconstructed[index] = value

    gaps = missing_ranges(covered)
    contiguous_prefix = 0
    while contiguous_prefix < len(covered) and covered[contiguous_prefix]:
        contiguous_prefix += 1
    complete = not gaps
    print(f"guest_path={args.guest_path}")
    print(f"source_size={len(source)}")
    print(f"source_sha256={source_hash}")
    print(f"matching_read_records={len(rows)} successful_records={successful_records}")
    print(f"transferred_bytes_including_overlaps={transferred} overlap_bytes={overlap_bytes}")
    print(f"contiguous_verified_prefix={contiguous_prefix} complete_file={str(complete).lower()}")
    if complete:
        reconstructed_hash = hashlib.sha256(reconstructed).hexdigest().upper()
        print(f"reconstructed_sha256={reconstructed_hash}")
        print(f"reconstructed_hash_matches_source={str(reconstructed_hash == source_hash).lower()}")
    else:
        prefix_hash = hashlib.sha256(reconstructed[:contiguous_prefix]).hexdigest().upper()
        print(f"verified_prefix_sha256={prefix_hash}")
        print(f"source_prefix_sha256={hashlib.sha256(source[:contiguous_prefix]).hexdigest().upper()}")
        print("gaps=" + ",".join(f"0x{start:x}-0x{end:x}" for start, end in gaps[:16]))
        if len(gaps) > 16:
            print(f"additional_gap_ranges={len(gaps) - 16}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
