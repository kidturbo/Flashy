# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline regression checks for exact-image draft generation and rejection paths."""
import copy
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

import e78_pedal_abc as draft
from e78_candidate import ORIGINAL


@unittest.skipUnless(ORIGINAL.is_file(), "Private exact baseline not supplied")
class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.profile = json.loads((draft.PROFILE).read_text())
        cls.model = draft.CalibrationImage()
        cls.result = draft.create_bytes(cls.profile, cls.model)

    def test_reference_midpoints_and_speed_taper(self):
        data = self.result[0]
        old = struct.unpack_from(">289f", self.model.original, 0x46944)
        new = struct.unpack_from(">289f", data, 0x46944)
        # Row50→56 is exactly half of the original50→62 interval at speed40.
        self.assertEqual(
            new[12 * 17 + 5], draft.f32((old[12 * 17 + 5] + old[13 * 17 + 5]) / 2)
        )
        # Row62→68 similarly takes half of62→74; speed20 takes half of that change.
        self.assertEqual(
            new[13 * 17 + 5], draft.f32((old[13 * 17 + 5] + old[14 * 17 + 5]) / 2)
        )
        self.assertEqual(
            new[13 * 17 + 3],
            draft.f32(0.75 * old[13 * 17 + 3] + 0.25 * old[14 * 17 + 3]),
        )

    def test_exact_scope_equality_and_checksums(self):
        data, report, plan, cells, _ = self.result
        self.assertEqual(data[0x46944:0x46DC8], data[0x46DC8:0x4724C])
        self.assertEqual(data[0x46944:0x46DC8], data[0x4724C:0x476D0])
        for table, address in draft.TABLES.items():
            for row, pedal in enumerate(draft.PEDALS):
                for col, speed in enumerate(draft.SPEEDS):
                    if pedal <= 20 or pedal == 100 or speed <= 10:
                        a = address + 4 * (row * 17 + col)
                        self.assertEqual(
                            data[a : a + 4], self.model.original[a : a + 4]
                        )
        self.assertEqual(data[0x476D0:0x47B54], self.model.original[0x476D0:0x47B54])
        self.assertEqual(plan["changed_blocks"], [6])
        self.assertEqual(len(report["checks"]), 17)
        self.assertTrue(all(c["passed"] for c in report["checks"]))
        self.assertEqual(sum(c["changed"] for c in cells), 3 * 9 * 14)
        allowed = self.model.checksum_offsets | set(range(0x46944, 0x476D0))
        self.assertTrue(
            all(
                i in allowed
                for i, (a, b) in enumerate(zip(self.model.original, data))
                if a != b
            )
        )
        self.assertFalse(report["flash_ready"])
        self.assertFalse(report["complete_aggressive_power_tune"])

    def test_profile_rejects_mismatch_nonfinite_low_pedal_endpoint_and_validation_claims(
        self,
    ):
        changes = [
            ("original_sha256", "0" * 64),
            ("tables", ["pedal_a"]),
            ("speed_weight", {"zero_through_kmh": 0, "full_from_kmh": 30}),
            ("flash_ready", True),
            ("vehicle_validated", True),
            ("complete_aggressive_power_tune", True),
        ]
        for key, value in changes:
            profile = copy.deepcopy(self.profile)
            profile[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                draft.validate_profile(profile)
        for index, value in [
            (0, 0.1),
            (6, 21),
            (16, 101),
            (12, float("nan")),
            (12, float("inf")),
            (12, 49),
            (12, 57),
            (10, 49),
        ]:
            profile = copy.deepcopy(self.profile)
            profile["stock_equivalent_pedal_percent"][index] = value
            with self.subTest(index=index, value=value), self.assertRaises(ValueError):
                draft.validate_profile(profile)

    def test_wrong_original_or_catalog_is_rejected(self):
        wrong = bytearray(self.model.original)
        wrong[0x46944] ^= 1
        with self.assertRaises(ValueError):
            draft.load_catalog(bytes(wrong))
        with (
            mock.patch.object(draft, "CATALOG_SHA", "0" * 64),
            self.assertRaises(ValueError),
        ):
            draft.load_catalog(self.model.original)

    def test_no_change_profile_produces_no_candidate(self):
        profile = copy.deepcopy(self.profile)
        profile["stock_equivalent_pedal_percent"] = list(draft.PEDALS)
        with tempfile.TemporaryDirectory() as temp:
            source, output = Path(temp) / "profile.json", Path(temp) / "output"
            source.write_text(json.dumps(profile))
            with self.assertRaises(ValueError):
                draft.build(source, output)
            self.assertFalse(output.exists())

    def test_immutable_saved_image_no_overwrite_and_no_selected_power_targets(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            result = draft.build(draft.PROFILE, output)
            image = output / "pedal-abc-response-draft.bin"
            self.assertEqual(image.read_bytes(), self.result[0])
            self.assertEqual(image.stat().st_mode & 0o777, 0o400)
            self.assertEqual(
                result["candidate_sha256"], draft.digest(image.read_bytes())
            )
            self.assertEqual(
                result["candidate_sha256"],
                "c2dd06b5134798bb836d10b5215bbac6f00acb57ee371ac9d279437cefead53f",
            )
            before = (output / "manifest.json").read_bytes()
            with self.assertRaises(FileExistsError):
                draft.build(draft.PROFILE, output)
            self.assertEqual(before, (output / "manifest.json").read_bytes())
            import csv

            with (output / "unselected-power-targets.csv").open() as stream:
                rows = list(csv.DictReader(stream))
            self.assertTrue(rows and all(r["proposed_value"] == "" for r in rows))


if __name__ == "__main__":
    unittest.main()
