# SPDX-License-Identifier: GPL-3.0-or-later
"""Definition export checks: invented-input rejection plus optional private roundtrips."""
import contextlib
import csv
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest

from export_table_definitions import SOURCE, SHA, build, export, read_values, sha


class PublicDefinitionGuards(unittest.TestCase):
    def test_synthetic_image_cannot_become_supported_baseline(self):
        for data in (b"", bytes(0x300000), bytes(0x300001)):
            with self.assertRaises(ValueError):
                build(data)

    def test_missing_private_baseline_has_no_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = root / "output"
            with self.assertRaises(FileNotFoundError):
                export(output, root / "missing-original.bin")
            self.assertFalse(output.exists())


@unittest.skipUnless(
    SOURCE.is_file(), "Private exact baseline not supplied; set E78_PRIVATE_BASELINE"
)
class PrivateDefinitionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = SOURCE.read_bytes()
        cls.report = build(cls.data)

    def test_every_cell_and_axis_roundtrips(self):
        total = 0
        self.assertEqual(len(self.report["tables"]), 10)
        for table in self.report["tables"]:
            count = table["count"]
            address = int(table["address"], 16)
            raw = struct.pack(
                ">" + str(count) + table["struct_code"], *table["raw_values"]
            )
            self.assertEqual(raw, self.data[address : address + len(raw)])
            self.assertEqual(sha(raw), table["sha256"])
            self.assertEqual(
                table["values"], [v * table["factor"] for v in table["raw_values"]]
            )
            self.assertFalse(table["editable"])
            self.assertFalse(table["flash_ready"])
            for axis in (table["row_axis"], table["column_axis"]):
                if axis:
                    packed = struct.pack(
                        ">" + str(axis["count"]) + "f", *axis["values"]
                    )
                    a = int(axis["address"], 16)
                    self.assertEqual(packed, self.data[a : a + len(packed)])
            total += count
        self.assertEqual(total, 1473)

    def test_exact_mismatch_even_one_byte_rejected(self):
        changed = bytearray(self.data)
        changed[-1] ^= 1
        with self.assertRaises(ValueError):
            build(bytes(changed))

    def test_export_hashes_csv_and_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "definitions"
            with contextlib.redirect_stdout(io.StringIO()):
                export(output)
            saved = json.loads((output / "definitions.json").read_text())
            self.assertEqual(saved["source_sha256"], SHA)
            self.assertFalse(saved["export_provenance"]["private_traces_bundled"])
            for line in (output / "SHA256SUMS").read_text().splitlines():
                digest, name = line.split("  ")
                self.assertEqual(sha((output / name).read_bytes()), digest)
            with (output / "table-cells.csv").open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 1473)
            tables = {t["id"]: t for t in saved["tables"]}
            for item in rows:
                table = tables[item["table"]]
                r, c = int(item["row_index"]), int(item["column_index"])
                index = r * table["shape"][1] + c
                address = int(table["address"], 16) + index * struct.calcsize(
                    table["struct_code"]
                )
                self.assertEqual(int(item["cell_address"], 16), address)
                self.assertEqual(float(item["value"]), table["values"][index])
            before = {p.name: p.read_bytes() for p in output.iterdir()}
            with self.assertRaises(FileExistsError):
                export(output)
            self.assertEqual({p.name: p.read_bytes() for p in output.iterdir()}, before)
            self.assertEqual(sha(SOURCE.read_bytes()), SHA)


if __name__ == "__main__":
    unittest.main()
