#!/usr/bin/env python3
"""Offline extraction for the verified 2026-11-09 read only; never writes ECU data.

Directory locations and segment labels: UniversalPatcher XML/e78.xml,
revision de421df36e195c3afc45e589aea0210afdf6beff (GPL-3.0).
This analysis utility is provided under GPL-3.0-or-later.
"""
import argparse
import hashlib
import json
from pathlib import Path

EXPECTED = "3b9b58421977ae2a5fb0f5d4d1bf71702cd97922109a215cab1ba9405e3920ba"
LAYOUT = [
    ("boot", 0x1ADC5, 0x1ADA0, 3, 12639237, None),
    ("os", 0xCB026, 0xCB000, 3, 12644082, "F64D"),
    ("system", 0xCB05A, 0x20000, 2, 55582024, "43E2"),
    ("fuel", 0xCB07E, 0x23000, 2, 55580794, "CBC9"),
    ("speedometer", 0xCB0A2, 0x28000, 2, 55580796, "0300"),
    ("engine-diagnostics", 0xCB0C6, 0x29000, 2, 55582022, "E755"),
    ("engine-operation", 0xCB0EA, 0x40000, 2, 55581437, "2929"),
]


def crc16(data):
    crc = 0
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def extract(source, output):
    data = source.read_bytes()
    if len(data) != 0x300000 or hashlib.sha256(data).hexdigest() != EXPECTED:
        raise ValueError("Input is not the verified original; no extraction performed")

    def pair(offset):
        start = int.from_bytes(data[offset : offset + 4], "big")
        end = int.from_bytes(data[offset + 4 : offset + 8], "big")
        if not 0 <= start <= end < len(data):
            raise ValueError("Invalid directory range")
        return start, end

    segments, artifacts = [], {}
    for name, directory, header, count, expected_pn, cvn in LAYOUT:
        start, end = pair(directory)
        pn = int.from_bytes(data[header + 5 : header + 9], "big")
        ranges = [pair(directory + 8 + i * 8) for i in range(count)]
        computed = crc16(b"".join(data[s : e + 1] for s, e in ranges))
        stored = data[header + 32 : header + 34]
        if pn != expected_pn or computed.to_bytes(2, "little") != stored:
            raise ValueError(f"{name}: identifier or CRC mismatch")
        if cvn is not None and stored.hex().upper() != cvn:
            raise ValueError(f"{name}: diagnostic CVN mismatch")
        segment = data[start : end + 1]
        record = dict(
            name=name,
            start=f"0x{start:06X}",
            end_inclusive=f"0x{end:06X}",
            bytes=len(segment),
            part_number=pn,
            sha256=hashlib.sha256(segment).hexdigest(),
            stored_crc=stored.hex().upper(),
            crc_verified=True,
            diagnostic_cvn=cvn,
            crc_ranges=[[hex(s), hex(e)] for s, e in ranges],
        )
        if name not in ("boot", "os"):
            filename = f"{name}-{pn}.bin"
            artifacts[filename] = segment
            record["file"] = filename
        segments.append(record)
    combined = data[0x20000:0x80000]
    artifacts["current-calibration-0x020000-0x07FFFF.bin"] = combined
    manifest = dict(
        source=str(source.resolve()),
        source_sha256=EXPECTED,
        os=12644082,
        classification="currently installed; factory stock unverified",
        segment_definition_revision="de421df36e195c3afc45e589aea0210afdf6beff",
        calibration_start="0x020000",
        calibration_end_inclusive="0x07FFFF",
        calibration_bytes=len(combined),
        calibration_sha256=hashlib.sha256(combined).hexdigest(),
        segments=segments,
        limitations=[
            "No table-level addresses, axes, or scaling defined",
            "No flash container or programming compatibility implied",
            "Wordsums and additional engine checksums not validated",
            "Two other diagnostic CALIDs are not mapped here",
        ],
    )
    # Exclusive new directory and files: never overwrite originals or earlier analysis.
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    for filename, content in artifacts.items():
        path = output / filename
        with path.open("xb") as stream:
            stream.write(content)
        path.chmod(0o400)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(
        json.dumps(
            dict(
                output=str(output),
                calibration_bytes=len(combined),
                verified_segment_crcs=len(segments),
            ),
            indent=2,
        )
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    extract(args.source, args.output)
