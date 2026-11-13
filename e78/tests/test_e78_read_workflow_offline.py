"""Fake application transport only: never opens USB or a CAN interface."""

from collections import deque
import hashlib
from pathlib import Path
import os
import tempfile
import unittest
from unittest.mock import patch

try:
    import lz4.block
except ModuleNotFoundError:
    lz4 = None
from e78_read_protocol import MAIN_FLASH_BYTES, ECC_SUBSTITUTIONS, upload_request
from e78_read_workflow import E78ReadWorkflow, ReadFailure, key_for_two_byte_seed

KERNEL = Path(
    os.environ.get(
        "E78_KERNEL_IMAGE",
        Path(__file__).resolve().parents[1] / "private/Kernel-E78.bin",
    )
)
VIN = "0" * 17  # Invented non-VIN fixture; never a vehicle identity.
CALIDS = ["12644082", "55582022"]
IDENTITY = {
    "reads": {
        "0902": {"vin": VIN},
        "0904": {"calibration_ids": CALIDS},
        "1ac9": {"gm_1ac9_identifier": 12639248},
    }
}
FIXTURE = bytearray(bytes(range(256)) * (MAIN_FLASH_BYTES // 256))
for _start, _end in ECC_SUBSTITUTIONS:
    FIXTURE[_start:_end] = b"\xff" * (_end - _start)
FIXTURE = bytes(FIXTURE)


class FakeECU:
    """Independent response generator with controllable failure cases."""

    def __init__(self, fault=None):
        self.fault = fault
        self.sent = []
        self.responses = deque()
        self.in_kernel = False
        self.pass_number = self.offset = self.next_sequence = 0
        self.compressed = False
        self.previous_request = self.previous_response = None
        self.retry_injected = False
        self.communication_disabled = False

    def send(self, payload, timeout):
        if timeout <= 0:
            raise AssertionError("Nonpositive send timeout")
        self.sent.append(payload)
        if self.fault == "pending_forever" and payload == b"\x09\x02":
            self.responses.append(b"\x7f\x09\x78")
            return
        if payload == b"\x09\x02":
            vin = VIN if self.fault != "identity" else "Z" * 17
            response = b"\x49\x02\x01" + vin.encode()
        elif payload == b"\x09\x04":
            response = (
                b"\x49\x04"
                + bytes((len(CALIDS),))
                + b"".join(x.encode().ljust(16, b"\0") for x in CALIDS)
            )
        elif payload == b"\x1a\xc9":
            response = b"\x5a\xc9" + (12639248).to_bytes(4, "big")
        elif payload == b"\x27\x01":
            if self.fault == "five_byte_seed":
                response = b"\x67\x01\x01\x02\x03\x04\x05"
            elif self.fault == "security_delay":
                response = b"\x7f\x27\x37"
            elif self.fault == "already_unlocked":
                response = b"\x67\x01\0\0"
            else:
                response = b"\x67\x01\x01\x23"
        elif payload.startswith(b"\x27\x02"):
            assert payload == b"\x27\x02\x29\x47"
            response = b"\x7f\x27\x35" if self.fault == "bad_key" else b"\x67\x02"
        elif payload == b"\x28":
            self.communication_disabled = self.fault != "quiet"
            response = b"\x68" if self.communication_disabled else b"\x7f\x28\x22"
        elif payload == b"\x10\x02":
            assert self.communication_disabled
            response = b"\x7f\x10\x22" if self.fault == "program" else b"\x50"
        elif payload in (b"\xa5\x01", b"\xa5\x03"):
            assert self.communication_disabled
            if self.fault == "a5_timeout":
                return
            response = b"\xe5"
        elif payload[0] == 0x34 and not self.in_kernel:
            assert len(payload) == 5 and payload[1] == 0
            response = b"\x74"
        elif payload.startswith(b"\x36\x00") and not self.in_kernel:
            address = int.from_bytes(payload[2:6], "big")
            assert 0x40014000 <= address < address + len(payload) - 6 <= 0x40020000
            response = b"\x7f\x36\x31" if self.fault == "ram" else b"\x76"
        elif payload == bytes.fromhex("368040014000") and not self.in_kernel:
            self.in_kernel = True
            self.responses.append(
                b"\x76"
            )  # Execution acknowledgement may precede ready.
            if self.fault == "no_ready":
                return
            response = b"\x99\x00" if self.fault == "bad_ready" else b"\x99"
        elif payload in (upload_request(), upload_request(True)) and self.in_kernel:
            self.pass_number += 1
            self.offset = 0
            self.next_sequence = 1
            self.previous_request = self.previous_response = None
            self.compressed = payload[1] == 0x10
            response = bytes.fromhex("75200fff")
        elif payload[0] == 0x36 and self.in_kernel:
            if payload == self.previous_request:
                self.responses.append(self.previous_response)
                return
            assert len(payload) == 2 and payload[1] == self.next_sequence
            if self.fault == "partial_failure" and self.offset:
                response = b"\x7f\x36\x72"
            else:
                count = 4096 if self.compressed else 4093
                block = FIXTURE[self.offset : self.offset + count]
                if (
                    self.fault == "mismatch"
                    and self.pass_number == 2
                    and self.offset == 0
                ):
                    block = bytes((block[0] ^ 1,)) + block[1:]
                response = b"\x76" + payload[1:]
                if self.compressed:
                    response += len(block).to_bytes(2, "big") + lz4.block.compress(
                        block, store_size=False
                    )
                else:
                    response += block
                self.previous_request, self.previous_response = payload, response
                self.offset += len(block)
                self.next_sequence = (self.next_sequence + 1) & 255
                if self.fault in ("lost_reply", "busy") and not self.retry_injected:
                    self.retry_injected = True
                    if self.fault == "busy":
                        self.responses.append(b"\x7f\x36\x21")
                    return
        elif payload == b"\x37" and self.in_kernel:
            assert self.offset == MAIN_FLASH_BYTES
            response = b"\x7f\x37\x24" if self.fault == "exit" else b"\x77"
        else:
            raise AssertionError(f"Unexpected request: {payload[:8].hex()}")
        self.responses.append(response)

    def recv(self, timeout):
        if not self.responses:
            raise TimeoutError("Synthetic no response")
        return self.responses.popleft()


@unittest.skipUnless(
    KERNEL.is_file() and lz4 is not None,
    "Optional published pinned kernel absent; set E78_KERNEL_IMAGE",
)
class WorkflowTests(unittest.TestCase):
    def run_workflow(self, fault=None, compressed=True):
        ecu = FakeECU(fault)
        with tempfile.TemporaryDirectory() as root:
            output = Path(root) / "read"
            workflow = E78ReadWorkflow(
                ecu, KERNEL.read_bytes(), IDENTITY, output, compressed=compressed
            )
            report = workflow.run()
            files = {p.name: p.read_bytes() for p in output.glob("*.bin")}
        self.assertFalse(any(p[0] in (0x11, 0x31, 0x2E) for p in ecu.sent))
        return report, ecu, files

    def test_published_two_byte_key_vectors_and_size_rejection(self):
        for seed, key in [("0123", "2947"), ("1234", "b1cf"), ("ffff", "a835")]:
            self.assertEqual(key_for_two_byte_seed(bytes.fromhex(seed)).hex(), key)
        with self.assertRaises(ReadFailure):
            key_for_two_byte_seed(bytes(5))

    def test_full_raw_and_lz4_reads_match_and_wrap_counter(self):
        for compressed in (False, True):
            with self.subTest(compressed=compressed):
                report, ecu, files = self.run_workflow(compressed=compressed)
                self.assertTrue(report["complete"], report.get("error"))
                self.assertEqual(
                    report["verified_sha256"], hashlib.sha256(FIXTURE).hexdigest()
                )
                self.assertEqual(files, {"pass-1.bin": FIXTURE, "pass-2.bin": FIXTURE})
                self.assertTrue(report["power_cycle_may_be_required"])
                self.assertFalse(report["stock_operation_restored"])
                self.assertIn(b"\x36\x00", ecu.sent)

    def test_identity_mismatch_prevents_security_and_ram(self):
        report, ecu, _ = self.run_workflow("identity")
        self.assertFalse(report["complete"])
        self.assertEqual(ecu.sent, [b"\x09\x02"])
        self.assertFalse(report["restoration_required"])

    def test_unsupported_or_rejected_security_never_retries_or_programs(self):
        for fault in ("five_byte_seed", "security_delay", "bad_key"):
            with self.subTest(fault=fault):
                report, ecu, _ = self.run_workflow(fault)
                self.assertFalse(report["complete"])
                self.assertEqual(ecu.sent.count(b"\x27\x01"), 1)
                self.assertEqual(
                    sum(p[:2] == b"\x27\x02" for p in ecu.sent), int(fault == "bad_key")
                )
                self.assertNotIn(b"\x10\x02", ecu.sent)

    def test_failed_program_or_ram_never_executes(self):
        for fault in ("program", "ram"):
            report, ecu, _ = self.run_workflow(fault)
            self.assertFalse(report["complete"])
            self.assertNotIn(bytes.fromhex("368040014000"), ecu.sent)
            self.assertFalse(ecu.in_kernel)

    def test_communication_control_precedes_program_and_rejection_stops(self):
        report, ecu, _ = self.run_workflow()
        self.assertTrue(report["complete"])
        self.assertLess(ecu.sent.index(b"\x28"), ecu.sent.index(b"\x10\x02"))
        report, ecu, _ = self.run_workflow("quiet")
        self.assertFalse(report["complete"])
        self.assertNotIn(b"\x10\x02", ecu.sent)
        self.assertFalse(report["power_cycle_may_be_required"])
        report, _, _ = self.run_workflow("program")
        self.assertFalse(report["power_cycle_may_be_required"])

    def test_exact_ready_required_before_read(self):
        for fault in ("no_ready", "bad_ready"):
            report, ecu, _ = self.run_workflow(fault)
            self.assertFalse(report["complete"])
            self.assertFalse(any(p[0] == 0x35 for p in ecu.sent))

    def test_already_unlocked_and_source_documented_a5_timeout(self):
        for fault in ("already_unlocked", "a5_timeout"):
            report, ecu, _ = self.run_workflow(fault)
            self.assertTrue(report["complete"], report.get("error"))
            if fault == "already_unlocked":
                self.assertFalse(any(p[:2] == b"\x27\x02" for p in ecu.sent))

    def test_lost_reply_or_busy_retries_same_counter_without_duplicate_data(self):
        for fault in ("lost_reply", "busy"):
            report, ecu, files = self.run_workflow(fault)
            self.assertTrue(report["complete"], report.get("error"))
            self.assertEqual(files["pass-1.bin"], FIXTURE)
            first = ecu.sent.index(b"\x36\x01")
            self.assertEqual(ecu.sent[first : first + 2], [b"\x36\x01"] * 2)

    def test_mismatch_and_missing_exit_do_not_produce_verified_dump(self):
        for fault in ("mismatch", "exit"):
            report, _, files = self.run_workflow(fault)
            self.assertFalse(report["complete"])
            self.assertNotIn("verified_sha256", report)
            self.assertTrue(files)

    def test_partial_data_is_preserved_and_never_called_complete(self):
        report, _, files = self.run_workflow("partial_failure")
        self.assertFalse(report["complete"])
        self.assertEqual(files, {"pass-1.partial.bin": FIXTURE[:4096]})

    def test_guard_rejects_erase_reset_and_kernel_download(self):
        with tempfile.TemporaryDirectory() as root:
            w = E78ReadWorkflow(
                FakeECU(), KERNEL.read_bytes(), IDENTITY, Path(root) / "read"
            )
            for stage in ("identity", "unlock", "program", "ram", "execute", "read"):
                w.stage = stage
                for payload in (
                    b"\x31\x01\xff\x01",
                    b"\x11\x01",
                    bytes.fromhex("3400440000000000300000"),
                ):
                    with self.assertRaises(ReadFailure):
                        w.guard(payload)

    def test_pending_responses_cannot_extend_overall_deadline(self):
        class PendingECU(FakeECU):
            def recv(self, timeout):
                return b"\x7f\x09\x78"

        clock = [0.0]

        def tick():
            clock[0] += 0.01
            return clock[0]

        ecu = PendingECU()
        with (
            tempfile.TemporaryDirectory() as root,
            patch("e78_read_workflow.time.monotonic", tick),
        ):
            w = E78ReadWorkflow(
                ecu,
                KERNEL.read_bytes(),
                IDENTITY,
                Path(root) / "read",
                budget_seconds=0.1,
            )
            report = w.run()
        self.assertFalse(report["complete"])
        self.assertEqual(ecu.sent, [b"\x09\x02"])
        self.assertIn("TimeoutError", report["error"])


if __name__ == "__main__":
    unittest.main()
