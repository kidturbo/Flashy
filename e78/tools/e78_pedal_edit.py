#!/usr/bin/env python3
"""Offline pedal-A CSV worksheet and exact-image candidate builder. No ECU I/O."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct

from e78_candidate import CalibrationImage, digest
from e78_programming import make_plan

ADDRESS, ROW_AXIS, COLUMN_AXIS, N = 0x46944, 0x47C1C, 0x47D0C, 17
FIELDS = (
    "row",
    "column",
    "pedal_percent",
    "speed_kmh",
    "original_kw",
    "proposed_kw",
    "reason",
)


def baseline(model):
    data = model.original
    return (
        struct.unpack_from(">17f", data, ROW_AXIS),
        struct.unpack_from(">17f", data, COLUMN_AXIS),
        struct.unpack_from(">289f", data, ADDRESS),
    )


def export(output):
    model = CalibrationImage()
    rows, columns, values = baseline(model)
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    with (output / "pedal-a.csv").open("x", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        for r, pedal in enumerate(rows):
            for c, speed in enumerate(columns):
                value = values[r * N + c]
                writer.writerow(
                    dict(
                        row=r,
                        column=c,
                        pedal_percent=pedal,
                        speed_kmh=speed,
                        original_kw=value,
                        proposed_kw=value,
                        reason="",
                    )
                )
    (output / "baseline.json").write_text(
        json.dumps(
            dict(
                original_sha256=digest(model.original),
                table="pedal_a",
                address=ADDRESS,
                shape=[N, N],
                unit="kW",
                changes=0,
                scope="Offline editable research copy. Proposed values initially equal the installed calibration.",
                runtime_scope="A selected; released samples and one nonzero stationary hold matched. Whole map unvalidated.",
                flash_ready=False,
            ),
            indent=2,
        )
        + "\n"
    )
    return output / "pedal-a.csv"


def apply_rows(records, model):
    rows, columns, values = baseline(model)
    if len(records) != N * N:
        raise ValueError("Worksheet must contain all 289 cells exactly once")
    seen, changes = set(), []
    data = bytearray(model.original)
    for item in records:
        if set(item) != set(FIELDS):
            raise ValueError("Worksheet columns changed")
        r, c = int(item["row"]), int(item["column"])
        if (r, c) in seen or not 0 <= r < N or not 0 <= c < N:
            raise ValueError("Duplicate or out-of-range cell")
        seen.add((r, c))
        old = values[r * N + c]
        if (
            float(item["pedal_percent"]) != rows[r]
            or float(item["speed_kmh"]) != columns[c]
            or float(item["original_kw"]) != old
        ):
            raise ValueError("Axes or original values differ from exact-image baseline")
        proposed = float(item["proposed_kw"])
        if not math.isfinite(proposed) or not min(values) <= proposed <= max(values):
            raise ValueError(
                "Proposed value outside original table range; no ceiling expansion supported"
            )
        packed = struct.pack(">f", proposed)
        new = struct.unpack(">f", packed)[0]
        if new == old:
            continue
        reason = item["reason"].strip()
        if not reason:
            raise ValueError("Each changed cell requires a reason")
        address = ADDRESS + 4 * (r * N + c)
        data[address : address + 4] = packed
        changes.append(
            dict(
                row=r,
                column=c,
                pedal_percent=rows[r],
                speed_kmh=columns[c],
                address=address,
                old_kw=old,
                requested_kw=proposed,
                stored_kw=new,
                delta_kw=new - old,
                reason=reason,
            )
        )
    # The existing audited corrector enforces calibration scope and all17 checks.
    corrected, report = model.correct(bytes(data))
    allowed = model.checksum_offsets | set(range(ADDRESS, ADDRESS + N * N * 4))
    if any(
        a not in allowed
        for a, (old, new) in enumerate(zip(model.original, corrected))
        if old != new
    ):
        raise ValueError("Unexpected change outside pedal-A data/checksum storage")
    report.update(
        table="pedal_a",
        cell_changes=changes,
        flash_ready=False,
        full_runtime_validation_complete=False,
        limits=[
            "Offline file only; no safe 92-AKI tune is inferred.",
            "Original numeric range is an input guard, not proof of engine or transmission safety.",
            "Only table A is edited; B/C/fallback, axes and protection tables are preserved.",
        ],
    )
    return corrected, report, make_plan(corrected, model)


def build(worksheet, output):
    with worksheet.open(newline="") as stream:
        records = list(csv.DictReader(stream))
    data, report, plan = apply_rows(records, CalibrationImage())
    if not report["cell_changes"]:
        raise ValueError(
            "No requested edits: worksheet is still identical to the original; no candidate created"
        )
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    image = output / "pedal-a-research-candidate.bin"
    with image.open("xb") as stream:
        stream.write(data)
    image.chmod(0o400)
    if digest(image.read_bytes()) != report["output_sha256"]:
        raise OSError("Saved candidate hash mismatch")
    shutil.copyfile(worksheet, output / "requested-edits.csv")
    (output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    (output / "block-plan.json").write_text(json.dumps(plan, indent=2) + "\n")
    (output / "SHA256SUMS").write_text(
        "".join(
            f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n"
            for p in sorted(output.iterdir())
            if p.is_file()
        )
    )
    return dict(
        changed_cells=len(report["cell_changes"]),
        checks_passed=len(report["checks"]),
        changed_blocks=plan["changed_blocks"],
        flash_ready=False,
        output=str(output),
    )


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    commands = p.add_subparsers(dest="command", required=True)
    e = commands.add_parser("export")
    e.add_argument("output", type=Path)
    b = commands.add_parser("build")
    b.add_argument("worksheet", type=Path)
    b.add_argument("output", type=Path)
    args = p.parse_args()
    print(
        export(args.output)
        if args.command == "export"
        else json.dumps(build(args.worksheet, args.output), indent=2)
    )
