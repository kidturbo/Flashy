"""Synthetic mutations on temporary copies; no hardware and no tuning proposal."""

import contextlib
import io
from pathlib import Path
import random
import tempfile
import unittest

from e78_candidate import (
    ORIGINAL,
    CalibrationImage,
    write_candidate,
    changed_ranges,
    digest,
    EXPECTED,
)
from extract_verified_calibration import crc16 as independent_crc


@unittest.skipUnless(
    ORIGINAL.is_file(), "Private exact baseline not supplied; set E78_PRIVATE_BASELINE"
)
class CandidateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.model = CalibrationImage()
        cls.original = cls.model.original

    def assert_independent_checks(self, data):
        for r in self.model.records:
            raw = b"".join(data[a : b + 1] for a, b in r["ranges"])
            if r["method"] == "crc16":
                value = independent_crc(raw)
            else:
                value = (
                    -sum(
                        int.from_bytes(raw[i : i + 2], "big")
                        for i in range(0, len(raw), 2)
                    )
                ) % (1 << (r["bytes"] * 8))
            self.assertEqual(
                value,
                int.from_bytes(
                    data[r["address"] : r["address"] + r["bytes"]], r["byteorder"]
                ),
            )

    def test_original_noop_and_idempotence(self):
        out, report = self.model.correct(self.original)
        self.assertEqual(out, self.original)
        self.assertEqual(report["checksum_changes"], [])

    def test_nested_engine_correction_and_reversal(self):
        mutated = bytearray(self.original)
        mutated[0x46944] ^= 1
        out, report = self.model.correct(mutated)
        self.assertEqual(
            set(r["name"] for r in report["checksum_changes"]),
            {"engine-extra-3", "engine-operation-crc16", "engine-operation-wordsum"},
        )
        self.assertEqual(out[0x46944], mutated[0x46944])
        self.assert_independent_checks(out)
        self.assertEqual(self.model.correct(out)[0], out)
        reverse = bytearray(out)
        reverse[0x46944] = self.original[0x46944]
        self.assertEqual(self.model.correct(reverse)[0], self.original)

    def test_all_calibration_segments_and_checksum_storage(self):
        mutated = bytearray(self.original)
        for address in (
            0x20034,
            0x23034,
            0x28034,
            0x29034,
            0x40034,
            0x4315C,
            0x464F0,
            0x60000,
            0x7FFFF,
        ):
            mutated[address] ^= 0x31
        for r in self.model.cal_records:
            mutated[r["address"]] ^= 0x80
        out, report = self.model.correct(mutated)
        self.assertTrue(all(r["passed"] for r in report["checks"]))
        self.assert_independent_checks(out)
        self.assertTrue(
            all(
                a in self.model.checksum_offsets
                for s, e in changed_ranges(mutated, out)
                for a in range(s, e)
            )
        )

    def test_reject_boot_os_metadata_and_bad_length(self):
        for address in (0, 0x1FFFF, 0x80000, 0xCB020, 0x2FFFFF, 0x20005, 0x40033):
            mutated = bytearray(self.original)
            mutated[address] ^= 1
            with self.assertRaises(ValueError):
                self.model.correct(mutated)
        for candidate in (self.original[:-1], self.original + b"\0"):
            with self.assertRaises(ValueError):
                self.model.correct(candidate)

    def test_random_interior_changes_preserved(self):
        rng = random.Random(12644082)
        mutated = bytearray(self.original)
        points = rng.sample(range(0x50000, 0x7FFFF), 100)
        for address in points:
            mutated[address] ^= rng.randrange(1, 256)
        out, _ = self.model.correct(mutated)
        self.assertTrue(all(out[a] == mutated[a] for a in points))
        self.assert_independent_checks(out)

    def test_exclusive_output_and_original_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "input.bin"
            source.write_bytes(self.original)
            output = Path(tmp) / "output"
            with contextlib.redirect_stdout(io.StringIO()):
                write_candidate(source, output)
            self.assertEqual(
                digest((output / "corrected-calibration-research.bin").read_bytes()),
                EXPECTED,
            )
            with self.assertRaises(FileExistsError):
                write_candidate(source, output)
            self.assertEqual(source.read_bytes(), self.original)


if __name__ == "__main__":
    unittest.main()
