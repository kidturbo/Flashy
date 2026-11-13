"""Offline checks of cell addressing, preserved bytes, checksums and worksheet guards."""

import copy
import csv
from pathlib import Path
import struct
import tempfile
import unittest

from e78_candidate import ORIGINAL, CalibrationImage
from e78_pedal_edit import ADDRESS, FIELDS, N, apply_rows, export, build


@unittest.skipUnless(
    ORIGINAL.is_file(), "Private exact baseline not supplied; set E78_PRIVATE_BASELINE"
)
class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.model = CalibrationImage()
        with tempfile.TemporaryDirectory() as tmp:
            worksheet = export(Path(tmp) / "worksheet")
            with worksheet.open(newline="") as stream:
                cls.records = list(csv.DictReader(stream))

    def test_unedited_roundtrip_and_no_spurious_candidate(self):
        data, report, plan = apply_rows(self.records, self.model)
        self.assertEqual(data, self.model.original)
        self.assertEqual(report["cell_changes"], [])
        self.assertEqual(plan["changed_blocks"], [])
        with tempfile.TemporaryDirectory() as tmp:
            worksheet = export(Path(tmp) / "worksheet")
            out = Path(tmp) / "candidate"
            with self.assertRaises(ValueError):
                build(worksheet, out)
            self.assertFalse(out.exists())

    def test_one_explicit_cell_corrects_checksums_preserves_other_data(self):
        records = copy.deepcopy(self.records)
        index = 6 * N + 3
        records[index]["proposed_kw"] = str(float(records[index]["original_kw"]) - 0.25)
        records[index][
            "reason"
        ] = "Synthetic offline test only, not a tune recommendation"
        data, report, plan = apply_rows(records, self.model)
        address = ADDRESS + index * 4
        self.assertEqual(
            struct.unpack_from(">f", data, address)[0],
            float(records[index]["proposed_kw"]),
        )
        allowed = self.model.checksum_offsets | set(range(address, address + 4))
        self.assertTrue(
            all(
                i in allowed
                for i, (a, b) in enumerate(zip(self.model.original, data))
                if a != b
            )
        )
        self.assertTrue(all(c["passed"] for c in report["checks"]))
        self.assertEqual(len(report["checks"]), 17)
        self.assertEqual(len(report["cell_changes"]), 1)
        self.assertEqual(plan["changed_blocks"], [6])
        self.assertFalse(report["flash_ready"])

    def test_worksheet_coordinate_baseline_reason_and_value_guards(self):
        for field, value in (
            ("row", "100"),
            ("original_kw", "1000"),
            ("speed_kmh", "1000"),
            ("proposed_kw", "nan"),
            ("proposed_kw", "9999"),
            ("proposed_kw", ".1"),
        ):
            records = copy.deepcopy(self.records)
            records[0][field] = value
            with self.assertRaises(ValueError):
                apply_rows(records, self.model)
        for records in (self.records[:-1], [self.records[0]] * (N * N)):
            with self.assertRaises(ValueError):
                apply_rows(records, self.model)

    def test_saved_candidate_is_fresh_readonly_and_manifested(self):
        with tempfile.TemporaryDirectory() as tmp:
            worksheet = export(Path(tmp) / "worksheet")
            records = copy.deepcopy(self.records)
            records[6 * N]["proposed_kw"] = "8.75"
            records[6 * N]["reason"] = "Synthetic test; never use in vehicle"
            with worksheet.open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=FIELDS)
                writer.writeheader()
                writer.writerows(records)
            out = Path(tmp) / "candidate"
            result = build(worksheet, out)
            self.assertEqual(result["changed_cells"], 1)
            self.assertEqual(
                (out / "pedal-a-research-candidate.bin").stat().st_mode & 0o777, 0o400
            )
            self.assertTrue((out / "SHA256SUMS").exists())
            with self.assertRaises(FileExistsError):
                build(worksheet, out)


if __name__ == "__main__":
    unittest.main()
