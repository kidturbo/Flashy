# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline checksum correction for calibration copies of this exact E78 original.

Uses pinned UniversalPatcher layout through the existing audit. No hardware I/O.
Passing these checks establishes file consistency, not calibration/flash safety.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path

from audit_e78_checksums import calculate, check, discover
from extract_verified_calibration import EXPECTED, LAYOUT

ROOT = Path(__file__).resolve().parents[1]
# A caller-owned private image is required; never add it to the repository.
ORIGINAL = Path(os.environ.get("E78_PRIVATE_BASELINE", ROOT / "private/original.bin"))
SIGNATURES = ROOT / "vendor/SegmentSeek-e78.xml"
SIGNATURE_SHA = "7e6a179d371d54e36a355fdf2b7449d26ff83c80b0d475b053a7970ab64bd4b9"
START, END, SIZE = 0x20000, 0x80000, 0x300000


def digest(data):
    return hashlib.sha256(data).hexdigest()


def changed_ranges(before, after):
    if len(before) != len(after):
        raise ValueError("Cannot compare unequal image sizes")
    ranges = []
    start = None
    for i, (a, b) in enumerate(zip(before, after)):
        if a != b and start is None:
            start = i
        elif a == b and start is not None:
            ranges.append([start, i])
            start = None
    if start is not None:
        ranges.append([start, len(before)])
    return ranges


class CalibrationImage:
    def __init__(self, original=None, signatures=SIGNATURES):
        self.original = ORIGINAL.read_bytes() if original is None else bytes(original)
        if len(self.original) != SIZE or digest(self.original) != EXPECTED:
            raise ValueError("Baseline must be the verified unchanged original")
        if digest(signatures.read_bytes()) != SIGNATURE_SHA:
            raise ValueError("Signature source differs from pinned audit")
        self.records, _ = discover(self.original, signatures)
        if len(self.records) != 17 or not all(
            check(self.original, r) for r in self.records
        ):
            raise ValueError("Baseline checksum audit failed")
        self.cal_records = [r for r in self.records if START <= r["address"] < END]
        # Order every checksum after checksums whose storage its coverage includes.
        pending = list(self.cal_records)
        self.order = []
        while pending:
            ready = [
                r
                for r in pending
                if not any(
                    any(
                        max(s, q["address"]) <= min(e, q["address"] + q["bytes"] - 1)
                        for s, e in r["ranges"]
                    )
                    for q in pending
                    if q is not r
                )
            ]
            if not ready:
                raise ValueError("Cyclic checksum dependency")
            self.order.extend(ready)
            pending = [r for r in pending if r not in ready]
        self.checksum_offsets = {
            a
            for r in self.cal_records
            for a in range(r["address"], r["address"] + r["bytes"])
        }

    def validate_scope(self, candidate):
        if len(candidate) != SIZE:
            raise ValueError("Requires full 3 MiB image")
        if (
            candidate[:START] != self.original[:START]
            or candidate[END:] != self.original[END:]
        ):
            raise ValueError("Boot/OS/outside-calibration changes are prohibited")
        for _, _, header, _, _, _ in LAYOUT:
            if START <= header < END:
                for a in range(header, header + 0x34):
                    if (
                        a not in self.checksum_offsets
                        and candidate[a] != self.original[a]
                    ):
                        raise ValueError(
                            f"Protected segment metadata changed at {a:06X}"
                        )

    def verify(self, candidate):
        self.validate_scope(candidate)
        return [
            dict(
                name=r["name"],
                passed=check(candidate, r),
                stored=int.from_bytes(
                    candidate[r["address"] : r["address"] + r["bytes"]], r["byteorder"]
                ),
                computed=calculate(candidate, r),
            )
            for r in self.records
        ]

    def correct(self, candidate):
        self.validate_scope(candidate)
        result = bytearray(candidate)
        changes = []
        for r in self.order:
            a, n = r["address"], r["bytes"]
            new = calculate(result, r).to_bytes(n, r["byteorder"])
            old = bytes(result[a : a + n])
            result[a : a + n] = new
            if old != new:
                changes.append(
                    dict(name=r["name"], address=a, before=old.hex(), after=new.hex())
                )
        result = bytes(result)
        checks = self.verify(result)
        if not all(r["passed"] for r in checks):
            raise ValueError("Post-correction verification failed")
        if any(
            a not in self.checksum_offsets
            for s, e in changed_ranges(candidate, result)
            for a in range(s, e)
        ):
            raise ValueError("Correction changed a non-checksum byte")
        return result, dict(
            original_sha256=EXPECTED,
            input_sha256=digest(candidate),
            output_sha256=digest(result),
            checksum_changes=changes,
            checks=checks,
            correction_order=[r["name"] for r in self.order],
            changes_from_original_end_exclusive=changed_ranges(self.original, result),
            flash_ready=False,
            scope="Offline calibration-file consistency only; table edits and hardware programming require separate validation.",
        )


def write_candidate(source, output):
    model = CalibrationImage()
    data = source.read_bytes()
    corrected, report = model.correct(data)
    # Fresh directory prevents accidental replacement of originals or other work.
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    target = output / "corrected-calibration-research.bin"
    with target.open("xb") as f:
        f.write(corrected)
        f.flush()
        os.fsync(f.fileno())
    target.chmod(0o400)
    if digest(target.read_bytes()) != report["output_sha256"]:
        raise OSError("Saved candidate hash mismatch")
    (output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            dict(
                output=str(output),
                checks_passed=17,
                checksum_fields_changed=len(report["checksum_changes"]),
                flash_ready=False,
            ),
            indent=2,
        )
    )


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    p.add_argument("output", type=Path)
    args = p.parse_args()
    write_candidate(args.source, args.output)
