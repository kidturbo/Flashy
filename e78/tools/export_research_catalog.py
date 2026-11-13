#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Export 108 research records from a caller's exact private image, offline."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import struct

from e78_candidate import ORIGINAL
from extract_verified_calibration import EXPECTED

ROOT = Path(__file__).resolve().parents[1]
METADATA = ROOT / "definitions/research-catalog.json"
METADATA_SHA256 = "6db2bcdbddfc9434aec467457d42920ae49279a70747212026656af146de97ad"


def build(image, metadata=None):
    """Reconstruct values only after whole-image and per-record checks."""
    if len(image) != 0x300000 or hashlib.sha256(image).hexdigest() != EXPECTED:
        raise ValueError("Exact private baseline required; no family-wide fallback")
    if (
        metadata is None
        and hashlib.sha256(METADATA.read_bytes()).hexdigest() != METADATA_SHA256
    ):
        raise ValueError("Definition metadata differs from the reviewed revision")
    source = json.loads(METADATA.read_text()) if metadata is None else metadata
    if source["original_sha256"] != EXPECTED or len(source["records"]) != 108:
        raise ValueError("Definition identity/count mismatch")
    records = []
    ids = set()
    for entry in source["records"]:
        ident = entry["id"]
        if ident in ids or entry["struct_code"] not in ("b", "B", "h", "H", "f"):
            raise ValueError("Duplicate record or unsupported storage")
        ids.add(ident)
        start = int(entry["address"], 16)
        shape = entry["shape"]
        if not shape or any(type(n) is not int or n <= 0 for n in shape):
            raise ValueError("Invalid definition dimensions")
        count = math.prod(shape)
        size = count * struct.calcsize(entry["struct_code"])
        if size != entry["byte_length"] or not 0 <= start < start + size <= len(image):
            raise ValueError("Definition length/range mismatch")
        data = image[start : start + size]
        if hashlib.sha256(data).hexdigest() != entry["byte_sha256"]:
            raise ValueError("Record bytes differ: " + ident)
        raw = list(struct.unpack(">" + str(count) + entry["struct_code"], data))
        if not all(math.isfinite(v) for v in raw):
            raise ValueError("Nonfinite record data")
        factor, offset = entry["factor"], entry["offset"]
        if factor is not None and not all(math.isfinite(v) for v in (factor, offset)):
            raise ValueError("Invalid definition conversion")
        record = dict(entry)
        record.update(
            raw_values=raw,
            values=[v * factor + offset for v in raw] if factor is not None else None,
            editable=False,
            flash_ready=False,
            full_vehicle_validation=False,
        )
        records.append(record)
    return dict(
        schema="e78-private-research-export-v1",
        original_sha256=EXPECTED,
        source_catalog_sha256=source["source_catalog_sha256"],
        record_count=len(records),
        logical_cells=sum(len(r["raw_values"]) for r in records),
        records=records,
        limitations=source["limitations"],
        editable=False,
        hardware_access=False,
        flash_ready=False,
    )


def export(source, output):
    result = build(source.read_bytes())
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    (output / "catalog.json").write_text(json.dumps(result, indent=2) + "\n")
    with (output / "cells.csv").open("x", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "record_id",
                "index",
                "address",
                "raw",
                "value",
                "unit",
                "proposed",
                "reason",
            ]
        )
        for record in result["records"]:
            width = struct.calcsize(record["struct_code"])
            for index, raw in enumerate(record["raw_values"]):
                value = record["values"][index] if record["values"] is not None else ""
                writer.writerow(
                    [
                        record["id"],
                        index,
                        f'{int(record["address"], 16) + width * index:06X}',
                        raw,
                        value,
                        record["unit"],
                        "",
                        "",
                    ]
                )
    print(
        json.dumps(
            {k: result[k] for k in ("record_count", "logical_cells", "flash_ready")}
        )
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--source", type=Path, default=ORIGINAL)
    args = parser.parse_args()
    export(args.source, args.output)
