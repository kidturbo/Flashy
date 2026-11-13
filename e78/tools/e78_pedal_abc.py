# SPDX-License-Identifier: GPL-3.0-or-later
#!/usr/bin/env python3
"""Exact-image offline A/B/C pedal draft; no adapter or live programming path."""
import argparse
import bisect
import csv
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "tools"))
from e78_candidate import CalibrationImage, digest
from e78_programming import make_plan

ORIGINAL_SHA = "3b9b58421977ae2a5fb0f5d4d1bf71702cd97922109a215cab1ba9405e3920ba"
CATALOG = ROOT / "definitions/research-catalog.json"
PROFILE = ROOT / "examples/pedal-abc-profile.json"
CATALOG_SHA = "6db2bcdbddfc9434aec467457d42920ae49279a70747212026656af146de97ad"
TABLES = {"pedal_a": 0x46944, "pedal_b": 0x46DC8, "pedal_c": 0x4724C}
PEDALS = (0, 2, 4, 8, 12, 16, 20, 24, 28, 32, 38, 44, 50, 62, 74, 86, 100)
SPEEDS = (2, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 140, 160, 180)
POWER_RECORDS = (
    "boost_pressure_limit",
    "indicated_peak_torque",
    "maximum_brake_torque",
    "overboost_torque",
    "spark_normal_minimum",
    "spark_fallback_minimum",
    "spark_temperature_offset",
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def f32(value):
    return struct.unpack(">f", struct.pack(">f", value))[0]


def validate_profile(profile):
    require(
        profile["schema"] == "e78-pedal-profile-v1"
        and profile["original_sha256"] == ORIGINAL_SHA,
        "Wrong profile schema or original",
    )
    require(
        profile["tables"] == list(TABLES), "Only the matched A/B/C set is supported"
    )
    require(
        profile["input_pedal_percent"] == list(PEDALS), "Original pedal knots required"
    )
    targets = profile["stock_equivalent_pedal_percent"]
    require(
        len(targets) == len(PEDALS)
        and all(isinstance(x, (int, float)) and math.isfinite(x) for x in targets),
        "Finite target at every original knot required",
    )
    require(
        all(a < b for a, b in zip(targets, targets[1:])),
        "Target progression must increase",
    )
    require(
        all(p <= target <= 100 for p, target in zip(PEDALS, targets)),
        "No lower requests or endpoint expansion",
    )
    require(
        all(p == target for p, target in zip(PEDALS, targets) if p <= 20 or p == 100),
        "Released/low/full pedal must remain stock",
    )
    require(
        all(target - p <= 6 for p, target in zip(PEDALS, targets)),
        "This experimental profile is bounded to six percentage points; not a safety limit",
    )
    require(
        profile["speed_weight"] == {"zero_through_kmh": 10, "full_from_kmh": 30},
        "Exact speed taper required",
    )
    require(
        profile["complete_aggressive_power_tune"] is False
        and profile["vehicle_validated"] is False
        and profile["flash_ready"] is False,
        "Cannot claim power/vehicle/flash validation",
    )
    return targets


def load_catalog(original):
    raw = CATALOG.read_bytes()
    require(digest(raw) == CATALOG_SHA, "Catalog changed")
    from export_research_catalog import build as reconstruct_catalog

    catalog = reconstruct_catalog(original)
    require(catalog["original_sha256"] == digest(original), "Catalog/image mismatch")
    for record in catalog["records"]:
        address = int(record["address"], 16)
        require(
            digest(original[address : address + record["byte_length"]])
            == record["byte_sha256"],
            "Catalog storage differs: " + record["id"],
        )
    return catalog


def interpolate_column(values, column, pedal):
    if pedal <= PEDALS[0]:
        return values[column]
    if pedal >= PEDALS[-1]:
        return values[16 * 17 + column]
    i = bisect.bisect_right(PEDALS, pedal) - 1
    a, b = values[i * 17 + column], values[(i + 1) * 17 + column]
    return a + (b - a) * (pedal - PEDALS[i]) / (PEDALS[i + 1] - PEDALS[i])


def create_bytes(profile, model):
    targets = validate_profile(profile)
    original = model.original
    catalog = load_catalog(original)
    require(
        struct.unpack_from(">17f", original, 0x47C1C) == PEDALS
        and struct.unpack_from(">17f", original, 0x47D0C) == SPEEDS,
        "Exact original axes required",
    )
    table_bytes = original[TABLES["pedal_a"] : TABLES["pedal_a"] + 1156]
    require(
        all(original[a : a + 1156] == table_bytes for a in TABLES.values()),
        "Original A/B/C equality required",
    )
    stock = struct.unpack(">289f", table_bytes)
    candidate, cells = bytearray(original), []
    for table, address in TABLES.items():
        for row, pedal in enumerate(PEDALS):
            for col, speed in enumerate(SPEEDS):
                weight = max(0.0, min(1.0, (speed - 10) / 20))
                effective = pedal + weight * (targets[row] - pedal)
                old = stock[row * 17 + col]
                new = f32(interpolate_column(stock, col, effective))
                require(
                    old - 1e-6 <= new <= stock[16 * 17 + col] + 1e-6,
                    "Request leaves stock per-speed endpoint envelope",
                )
                offset = address + 4 * (row * 17 + col)
                candidate[offset : offset + 4] = struct.pack(">f", new)
                cells.append(
                    dict(
                        table=table,
                        row=row,
                        column=col,
                        address=f"{offset:06X}",
                        pedal_percent=pedal,
                        speed_kmh=speed,
                        speed_weight=weight,
                        stock_equivalent_at_this_knot=effective,
                        original_kw=old,
                        proposed_kw=new,
                        delta_kw=new - old,
                        changed=new != old,
                    )
                )
    corrected, report = model.correct(bytes(candidate))
    allowed = model.checksum_offsets | {
        a for base in TABLES.values() for a in range(base, base + 1156)
    }
    require(
        all(
            a in allowed
            for a, (old, new) in enumerate(zip(original, corrected))
            if old != new
        ),
        "Unexpected byte outside A/B/C and checksum storage",
    )
    require(
        any(c["changed"] for c in cells),
        "No-change profiles do not produce a candidate",
    )
    plan = make_plan(corrected, model)
    require(
        plan["changed_blocks"] == [6], "Only physical calibration block6 may differ"
    )
    report.update(
        name=profile["name"],
        profile=profile,
        changed_cells=sum(c["changed"] for c in cells),
        changed_bytes=sum(a != b for a, b in zip(original, corrected)),
        complete_aggressive_power_tune=False,
        vehicle_validated=False,
        hardware_access=False,
        flash_ready=False,
        limitations=[
            "Experimental pedal-only draft; no output gain or combustion safety is established.",
            "Shape parameters are selected draft choices, not validated engine/transmission limits.",
            "A/B/C equality is retained; complete modifier/fallback/transition dynamics are unvalidated.",
            "No high-load targets are selected; 92 AKI/exhaust/location are context only.",
            "The earlier simulator programming run used r01, not this new r02 image.",
        ],
    )
    return corrected, report, plan, cells, catalog


def write_csv(path, records):
    with path.open("x", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(records[0]))
        writer.writeheader()
        writer.writerows(records)


def build(profile_path, output):
    # Validate and correct completely before creating an output directory.
    profile = json.loads(profile_path.read_text())
    data, report, plan, cells, catalog = create_bytes(profile, CalibrationImage())
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    image = output / "pedal-abc-response-draft.bin"
    with image.open("xb") as stream:
        stream.write(data)
    image.chmod(0o400)
    require(
        digest(image.read_bytes()) == report["output_sha256"],
        "Saved image digest mismatch",
    )
    (output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    (output / "block-plan.json").write_text(json.dumps(plan, indent=2) + "\n")
    (output / "profile.json").write_bytes(profile_path.read_bytes())
    write_csv(output / "pedal-cells.csv", cells)
    write_csv(output / "changed-cells.csv", [c for c in cells if c["changed"]])
    power = []
    for record in catalog["records"]:
        if record["id"] not in POWER_RECORDS:
            continue
        width = struct.calcsize(">" + record["struct_code"])
        for index, value in enumerate(record["values"]):
            power.append(
                dict(
                    record=record["id"],
                    role=record["name"],
                    index=index,
                    address=f"{int(record['address'],16)+width*index:06X}",
                    unit=record["unit"],
                    original_value=value,
                    proposed_value="",
                    decision="No power-target edit selected",
                    reason="Missing justified loaded target; a spark minimum is not the main spark target",
                )
            )
    write_csv(output / "unselected-power-targets.csv", power)
    sources = {
        "tool": Path(__file__).resolve(),
        "profile": profile_path,
        "catalog_metadata": CATALOG,
        "candidate_core": ROOT / "tools/e78_candidate.py",
        "programming_core": ROOT / "tools/e78_programming.py",
        "checksum_core": ROOT / "tools/audit_e78_checksums.py",
    }
    (output / "source-hashes.json").write_text(
        json.dumps(
            {label: digest(path.read_bytes()) for label, path in sources.items()},
            indent=2,
        )
        + "\n"
    )
    (output / "SHA256SUMS").write_text(
        "".join(
            f"{digest(f.read_bytes())}  {f.name}\n"
            for f in sorted(output.iterdir())
            if f.is_file()
        )
    )
    return dict(
        candidate_sha256=report["output_sha256"],
        changed_cells=report["changed_cells"],
        changed_bytes=report["changed_bytes"],
        checks=len(report["checks"]),
        changed_blocks=plan["changed_blocks"],
        complete_aggressive_power_tune=False,
        flash_ready=False,
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    print(json.dumps(build(args.profile, args.output), indent=2))
