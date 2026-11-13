# SPDX-License-Identifier: GPL-3.0-or-later
"""Metadata/decoder checks and optional exact-image export verification."""
import copy
import csv
import json
from pathlib import Path
import tempfile
import unittest

from e78_candidate import ORIGINAL
from e78_identity import decode_result
from export_research_catalog import METADATA, build, export


class PublicResearchTests(unittest.TestCase):
    def test_metadata_has_no_stored_cells_and_unique_bounded_records(self):
        data = json.loads(METADATA.read_text())
        records = data["records"]
        self.assertEqual(len({r["id"] for r in records}), 108)
        for r in records:
            self.assertNotIn("raw_values", r)
            self.assertNotIn("values", r)
            self.assertFalse(r["editable"])
            self.assertFalse(r["flash_ready"])
            self.assertLessEqual(int(r["address"], 16) + r["byte_length"], 0x300000)

    def test_unknown_or_wrong_length_image_rejected(self):
        for image in (b"", bytes(0x300000), bytes(0x300001)):
            with self.assertRaises(ValueError):
                build(image)

    def test_identity_decodes_only_exact_supported_payloads(self):
        self.assertEqual(
            decode_result(b"\x09\x02", b"\x49\x02\x01" + b"0" * 17), {"vin": "0" * 17}
        )
        self.assertEqual(
            decode_result(b"\x1a\xc9", b"\x5a\xc9\x12\x34\x56\x78"),
            {"gm_1ac9_identifier": 0x12345678},
        )
        payload = b"\x49\x04\x01" + b"SYNTHETIC".ljust(16, b"\0")
        self.assertEqual(
            decode_result(b"\x09\x04", payload), {"calibration_ids": ["SYNTHETIC"]}
        )
        for request, response in (
            (b"\x09\x02", b"\x49\x02\x01"),
            (b"\x09\x04", b"\x49\x04\0"),
            (b"\x1a\xc9", b"\x5a\xc9"),
            (b"\x09\x06", b"\x49\x06\0"),
        ):
            with self.assertRaises(ValueError):
                decode_result(request, response)


@unittest.skipUnless(ORIGINAL.is_file(), "Private exact baseline not supplied")
class PrivateResearchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = ORIGINAL.read_bytes()

    def test_all_records_and_vvt_axes(self):
        result = build(self.image)
        self.assertEqual(result["record_count"], 108)
        self.assertEqual(result["logical_cells"], 15893)
        rows = {r["id"]: r for r in result["records"]}
        self.assertEqual(rows["spark_vcp_primary"]["shape"], [33, 17])
        self.assertEqual(rows["spark_vcp_mode_2_3"]["raw_values"], [0] * 561)
        self.assertIn("inline", rows["spark_vcp_primary"]["axis_binding"]["rate"])

    def test_bad_metadata_and_mutated_image_rejected(self):
        bad = copy.deepcopy(json.loads(METADATA.read_text()))
        bad["records"][0]["byte_sha256"] = "0" * 64
        with self.assertRaises(ValueError):
            build(self.image, bad)
        changed = bytearray(self.image)
        changed[0x7463D] ^= 1
        with self.assertRaises(ValueError):
            build(changed)

    def test_export_is_private_fresh_and_keeps_proposals_blank(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "export"
            export(ORIGINAL, output)
            self.assertEqual(output.stat().st_mode & 0o777, 0o700)
            data = json.loads((output / "catalog.json").read_text())
            self.assertFalse(data["flash_ready"])
            with (output / "cells.csv").open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 15893)
            self.assertTrue(all(row["proposed"] == row["reason"] == "" for row in rows))
            with self.assertRaises(FileExistsError):
                export(ORIGINAL, output)
