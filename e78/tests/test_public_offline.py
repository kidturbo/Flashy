# SPDX-License-Identifier: GPL-3.0-or-later
"""Public checks that require no firmware or vehicle connection."""
import hashlib
from pathlib import Path
import unittest

from audit_e78_checksums import calculate, check, crc16
from e78_candidate import CalibrationImage, SIGNATURES, SIGNATURE_SHA, changed_ranges
from e78_read_protocol import UploadDecoder
from synthetic_demo import run


class PublicTests(unittest.TestCase):
    def test_demo(self):
        result = run()
        self.assertTrue(result["synthetic"])
        self.assertTrue(result["unknown_baseline_rejected"])
        self.assertFalse(result["flash_ready"])

    def test_signature_provenance(self):
        self.assertEqual(
            hashlib.sha256(SIGNATURES.read_bytes()).hexdigest(), SIGNATURE_SHA
        )

    def test_crc_known_vector_and_mutation(self):
        self.assertEqual(crc16(b"123456789"), 0xBB3D)
        record = dict(
            method="crc16", address=9, bytes=2, byteorder="little", ranges=[(0, 8)]
        )
        data = b"123456789" + bytes.fromhex("3dbb")
        self.assertTrue(check(data, record))
        self.assertFalse(check(b"023456789" + data[9:], record))

    def test_wordsum_width_and_odd_length(self):
        record = dict(
            method="wordsum", address=4, bytes=4, byteorder="big", ranges=[(0, 3)]
        )
        self.assertEqual(
            calculate(bytes.fromhex("ffff000100000000"), record), 0xFFFF0000
        )
        record["ranges"] = [(0, 2)]
        with self.assertRaises(ValueError):
            calculate(bytes(8), record)

    def test_exact_baseline_guards(self):
        for data in (b"", bytes(0x300000), bytes(0x300001)):
            with self.assertRaises(ValueError):
                CalibrationImage(original=data)
        with self.assertRaises(ValueError):
            changed_ranges(b"0", b"00")

    def test_raw_transfer_requires_sequence_length_and_terminal_ack(self):
        decoder = UploadDecoder(bytes.fromhex("75200006"), size=4)
        self.assertEqual(decoder.next_request(), bytes.fromhex("3601"))
        with self.assertRaises(ValueError):
            decoder.accept(bytes.fromhex("760200010203"))
        with self.assertRaises(ValueError):
            decoder.accept(bytes.fromhex("76010001"))
        decoder.accept(bytes.fromhex("760100010203"))
        self.assertEqual(decoder.exit_request(), b"\x37")
        with self.assertRaises(ValueError):
            decoder.confirm_exit(b"\x76")
        self.assertEqual(decoder.confirm_exit(b"\x77"), bytes(range(4)))


if __name__ == "__main__":
    unittest.main()
