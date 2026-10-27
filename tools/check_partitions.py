#!/usr/bin/env python3
"""Validate partition alignment, overlap, flash bounds, and OTA slot sizing."""
from pathlib import Path
import csv
import sys

ROOT = Path(__file__).resolve().parents[1]
CSV_PATH = ROOT / "partitions.csv"
FLASH_SIZE = 0x400000
APP_ALIGNMENT = 0x10000
SECTOR_ALIGNMENT = 0x1000


def size_value(value: str) -> int:
    value = value.strip().lower().replace(" ", "")
    multiplier = 1
    if value.endswith("k"):
        multiplier, value = 1024, value[:-1]
    elif value.endswith("m"):
        multiplier, value = 1024 * 1024, value[:-1]
    return int(value, 0) * multiplier


def main() -> int:
    rows = []
    with CSV_PATH.open(newline="", encoding="utf-8") as f:
        for raw in csv.reader(line for line in f if not line.lstrip().startswith("#")):
            if not raw or not raw[0].strip():
                continue
            name, typ, subtype, offset_raw, size_raw, *_ = [x.strip() for x in raw] + [""] * (6 - len(raw))
            offset = size_value(offset_raw) if offset_raw else None
            size = size_value(size_raw)
            if offset is None:
                if not rows:
                    raise ValueError(f"First partition {name} must have an explicit offset")
                offset = rows[-1]["end"]
            alignment = APP_ALIGNMENT if typ == "app" else SECTOR_ALIGNMENT
            if offset % alignment:
                raise ValueError(f"{name}: offset {offset:#x} violates {alignment:#x} alignment")
            if size <= 0 or size % SECTOR_ALIGNMENT:
                raise ValueError(f"{name}: size {size:#x} is invalid / unaligned")
            end = offset + size
            if end > FLASH_SIZE:
                raise ValueError(f"{name}: ends at {end:#x}, beyond 4MB flash")
            if rows and offset < rows[-1]["end"]:
                raise ValueError(f"{name}: overlaps previous partition {rows[-1]['name']}")
            rows.append({"name": name, "type": typ, "subtype": subtype, "offset": offset, "size": size, "end": end})
    slots = {r["subtype"]: r for r in rows if r["type"] == "app" and r["subtype"] in {"ota_0", "ota_1"}}
    if set(slots) != {"ota_0", "ota_1"}:
        raise ValueError("Both ota_0 and ota_1 application slots are required")
    if slots["ota_0"]["size"] != slots["ota_1"]["size"]:
        raise ValueError("OTA slots must be equal size")
    if not any(r["type"] == "data" and r["subtype"] == "ota" and r["size"] == 0x2000 for r in rows):
        raise ValueError("An 8 KiB OTA data partition is required")
    print("Partition table OK")
    for r in rows:
        print(f"{r['name']:8} {r['offset']:#08x} .. {r['end'] - 1:#08x}  {r['size'] / 1024:7.1f} KiB  {r['type']}/{r['subtype']}")
    print(f"OTA app slot size: {slots['ota_0']['size']:,} bytes")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as exc:
        print(f"Partition check failed: {exc}", file=sys.stderr)
        sys.exit(1)
