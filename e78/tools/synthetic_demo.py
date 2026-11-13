#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run checksum examples on invented bytes; no firmware, files or hardware I/O."""
import json

from audit_e78_checksums import calculate, check, crc16
from e78_candidate import CalibrationImage, changed_ranges


def run():
    # Entirely invented eight-byte buffer: three words and two checksum bytes.
    data = bytearray.fromhex("0001000200030000")
    record = dict(
        method="wordsum", address=6, bytes=2, byteorder="big", ranges=[(0, 5)]
    )
    checksum = calculate(data, record)
    data[6:8] = checksum.to_bytes(2, "big")
    assert checksum == 0xFFFA and check(data, record)
    changed = bytearray(data)
    changed[1] ^= 1
    assert not check(changed, record)
    assert changed_ranges(data, changed) == [[1, 2]]
    assert crc16(b"123456789") == 0xBB3D
    rejected = False
    try:
        CalibrationImage(original=bytes(0x300000))
    except ValueError:
        rejected = True
    assert rejected
    return dict(
        synthetic=True,
        firmware_included=False,
        hardware_commands=0,
        crc16_check_vector="0xBB3D",
        synthetic_wordsum="0xFFFA",
        deliberate_mutation_detected=True,
        unknown_baseline_rejected=True,
        flash_ready=False,
        scope="Algorithm demonstration only; no ECU execution or tune validation",
    )


if __name__ == "__main__":
    print(json.dumps(run(), indent=2))
