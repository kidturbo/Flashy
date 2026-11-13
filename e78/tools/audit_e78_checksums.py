#!/usr/bin/env python3
"""Read-only audit of the verified OS12644082 original. GPL-3.0-or-later.

Layout/signatures: UniversalPatcher de421df36e195c3afc45e589aea0210afdf6beff.
No checksum repair or ECU communication is implemented.
"""
import argparse
import hashlib
import json
import re
import struct
import xml.etree.ElementTree as ET
from pathlib import Path
from extract_verified_calibration import EXPECTED, LAYOUT


def crc16(data):
    table = []
    for value in range(256):
        for _ in range(8):
            value = (value >> 1) ^ (0xA001 if value & 1 else 0)
        table.append(value)
    result = 0
    for value in data:
        result = (result >> 8) ^ table[(result ^ value) & 255]
    return result


def calculate(data, record):
    covered = b"".join(data[s : e + 1] for s, e in record["ranges"])
    if record["method"] == "crc16":
        return crc16(covered)
    if len(covered) % 2:
        raise ValueError("Odd wordsum length")
    return (-sum(v[0] for v in struct.iter_unpack(">H", covered))) & (
        (1 << (record["bytes"] * 8)) - 1
    )


def check(data, record):
    offset, size = record["address"], record["bytes"]
    return calculate(data, record) == int.from_bytes(
        data[offset : offset + size], record["byteorder"]
    )


def discover(data, signature_file):
    def pair(offset):
        start, end = struct.unpack_from(">II", data, offset)
        if not 0 <= start <= end < len(data):
            raise ValueError("Out-of-bounds range")
        return start, end

    records = []
    for name, directory, header, count, pn, _ in LAYOUT:
        if int.from_bytes(data[header + 5 : header + 9], "big") != pn:
            raise ValueError("Unexpected segment identifier")
        for method, address, begin, n, order in [
            ("crc16", header + 32, directory + 8, count, "little"),
            (
                "wordsum",
                header,
                directory + 8 + count * 8,
                2 if count == 3 else 1,
                "big",
            ),
        ]:
            records.append(
                dict(
                    name=f"{name}-{method}",
                    method=method,
                    address=address,
                    bytes=2,
                    byteorder=order,
                    ranges=[pair(begin + i * 8) for i in range(n)],
                )
            )
    found, evidence = {}, {}
    for entry in ET.parse(signature_file).getroot():
        name = entry.findtext("Name")
        tokens = entry.findtext("SearchStr").split()
        pattern = b"".join(
            b"." if t in ("*", "@") else re.escape(bytes.fromhex(t)) for t in tokens
        )
        hits = list(re.finditer(pattern, data, re.DOTALL))
        expected_hits = 1 if name.startswith("cs") else 2
        if len(hits) != expected_hits:
            raise ValueError(f"{name}: unexpected signature hit count {len(hits)}")
        hit = hits[int(entry.findtext("UseHit")) - 1].start()
        raw = bytes(data[hit + i] for i, t in enumerate(tokens) if t == "@")
        high, low = struct.unpack(">Hh", raw)
        address = (high << 16) + low
        if not 0 <= address < len(data):
            raise ValueError("Invalid instruction-derived address")
        found[name] = address
        evidence[name] = dict(
            signature_hit=hit, hits=len(hits), decoded_address=address
        )
    for i in range(1, 4):
        start, end, address = (found[f"{key}{i}"] for key in ("start", "end", "cs"))
        if not 0 <= start <= end < address <= len(data) - 4 or end + 1 != address:
            raise ValueError("Unexpected extra checksum layout")
        records.append(
            dict(
                name=f"engine-extra-{i}",
                method="wordsum",
                address=address,
                bytes=4,
                byteorder="big",
                ranges=[(start, end)],
            )
        )
    return records, evidence


def audit(source, signatures, output):
    data = source.read_bytes()
    if len(data) != 0x300000 or hashlib.sha256(data).hexdigest() != EXPECTED:
        raise ValueError("Requires the unchanged verified original")
    if crc16(b"123456789") != 0xBB3D:
        raise ValueError("CRC implementation self-check failed")
    records, evidence = discover(data, signatures)
    mutation_checks = 0
    for record in records:
        record["stored"] = int.from_bytes(
            data[record["address"] : record["address"] + record["bytes"]],
            record["byteorder"],
        )
        record["computed"] = calculate(data, record)
        record["pass"] = check(data, record)
        if not record["pass"]:
            raise ValueError(f"Checksum mismatch: {record['name']}")
        # In-memory negative tests: endpoints of every covered range and storage.
        for point in [p for bounds in record["ranges"] for p in bounds] + [
            record["address"]
        ]:
            changed = bytearray(data)
            changed[point] ^= 1
            if check(changed, record):
                raise ValueError(
                    f"Mutation was not detected: {record['name']} at {point:x}"
                )
            mutation_checks += 1
    report = dict(
        source_sha256=EXPECTED,
        os=12644082,
        signatures_sha256=hashlib.sha256(signatures.read_bytes()).hexdigest(),
        checksums=records,
        instruction_evidence=evidence,
        passed=len(records),
        mutation_checks_passed=mutation_checks,
        scope="All 17 checks specified by pinned E78 XML; not proof of exhaustive ECU validation or programming support",
    )
    with output.open("x") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(
        f"{len(records)} checksum matches; {mutation_checks} in-memory mutations detected; no binaries written"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("signatures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    audit(args.source, args.signatures, args.output)
