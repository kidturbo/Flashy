"""Memory-backed protocol peer with deterministic failures; no CAN/serial access."""

from collections import deque
import json
from pathlib import Path
import tempfile
import unittest

from e78_candidate import ORIGINAL, CalibrationImage, digest
from e78_programming import BLOCKS, KernelProgrammer, ProgrammingFailure, make_plan
from e78_read_protocol import KERNEL_SHA256


class KernelModel:
    def __init__(self, image, fault=None, maximum=4090):
        self.image = bytearray(image)
        self.fault = fault
        self.maximum = maximum
        self.queue = deque()
        self.requests = []
        self.inventory_count = 0
        self.transfer = None
        self.last_packet = None
        self.next_sequence = 1
        self.did_busy = False
        self.did_drop = False
        self.exit_busy = False
        self.accepted_chunks = 0
        self.erase_success = False

    def send(self, p, timeout):
        p = bytes(p)
        self.requests.append(p)
        if p == bytes.fromhex("3101ff00"):
            self.inventory_count += 1
            self.queue.append(bytes.fromhex("7f3178"))
            for ident, (address, size) in enumerate(BLOCKS):
                digest_bytes = bytes.fromhex(
                    digest(self.image[address : address + size])
                )
                if (
                    self.fault == "final_hash"
                    and self.inventory_count == 2
                    and ident == 6
                ):
                    digest_bytes = bytes(32)
                claimed = (
                    address + 8
                    if self.fault == "inventory_map" and ident == 6
                    else address
                )
                self.queue.append(
                    bytes.fromhex("7101ff001c")
                    + bytes((ident,))
                    + claimed.to_bytes(4, "big")
                    + size.to_bytes(4, "big")
                    + digest_bytes
                )
        elif p[:4] == bytes.fromhex("3101ff01"):
            ident = p[4]
            assert ident in (4, 5, 6, 7)
            self.queue.extend(
                bytes.fromhex("7101ff01") + bytes((ident, status)) for status in (0, 1)
            )
            if self.fault == "erase_timeout":
                return
            status = 3 if self.fault == "erase_failure" else 2
            self.queue.append(bytes.fromhex("7101ff01") + bytes((ident, status)))
            if status == 2:
                address, size = BLOCKS[ident]
                self.image[address : address + size] = b"\xff" * size
                self.erase_success = True
        elif p[0] in (0x34, 0x35):
            assert len(p) == 11 and p[1:3] == bytes.fromhex("0044")
            address, size = int.from_bytes(p[3:7], "big"), int.from_bytes(
                p[7:11], "big"
            )
            assert (address, size) in BLOCKS[4:8]
            if p[0] == 0x34:
                assert self.erase_success, "download before terminal erase success"
            self.transfer = (p[0], address, size, 0)
            self.next_sequence = 1
            self.last_packet = None
            self.queue.append(
                bytes((p[0] + 0x40, 0x20)) + self.maximum.to_bytes(2, "big")
            )
        elif p[0] == 0x36:
            assert self.transfer
            service, address, size, done = self.transfer
            if self.last_packet and p[1] == self.last_packet[1]:
                assert p == self.last_packet, "retry changed data"
                self.queue.append(self.last_reply)
                return
            assert p[1] == self.next_sequence
            if service == 0x34:
                assert len(p) <= self.maximum and (len(p) - 2) % 8 == 0
                if self.fault == "busy_and_drop" and not self.did_busy:
                    self.did_busy = True
                    self.queue.append(bytes.fromhex("7f3621"))
                    return
                if self.fault == "write_failure":
                    self.queue.append(bytes.fromhex("7f3672"))
                    return
                self.image[address + done : address + done + len(p) - 2] = p[2:]
                used = len(p) - 2
                reply = bytes(
                    (0x76, (p[1] + 1) & 255 if self.fault == "wrong_sequence" else p[1])
                )
                self.accepted_chunks += 1
            else:
                assert len(p) == 2
                used = min(size - done, self.maximum - 2)
                data = bytes(self.image[address + done : address + done + used])
                if self.fault == "readback_mismatch" and done == 0:
                    data = bytes((data[0] ^ 1,)) + data[1:]
                reply = bytes((0x76, p[1])) + data
            self.transfer = (service, address, size, done + used)
            self.next_sequence = (p[1] + 1) & 255
            self.last_packet = p
            self.last_reply = reply
            if self.fault == "busy_and_drop" and service == 0x34 and not self.did_drop:
                self.did_drop = True
                return
            self.queue.append(reply)
        elif p == b"\x37":
            assert self.transfer
            service, address, size, done = self.transfer
            assert done == size
            if service == 0x34 and self.fault == "missing_77":
                self.transfer = None
                return
            if service == 0x34 and not self.exit_busy:
                self.exit_busy = True
                self.queue.append(bytes.fromhex("7f3721"))
                return
            self.transfer = None
            self.queue.append(b"\x77")
        else:
            raise AssertionError(f"Unexpected command {p[:6].hex()}")

    def recv(self, timeout):
        if not self.queue:
            raise TimeoutError("Injected timeout")
        return self.queue.popleft()


@unittest.skipUnless(
    ORIGINAL.is_file(), "Private exact baseline not supplied; set E78_PRIVATE_BASELINE"
)
class ProgrammingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.model = CalibrationImage()
        changed = bytearray(cls.model.original)
        changed[0x46944] ^= 1  # Synthetic bit flip; never a suggested tune.
        cls.candidate = cls.model.correct(changed)[0]

    def run_case(self, fault=None, *, initial=None, maximum=4090, candidate=None):
        peer = KernelModel(
            initial if initial is not None else self.model.original, fault, maximum
        )
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        journal = Path(tmp.name) / "journal.jsonl"
        client = KernelProgrammer(
            peer,
            candidate or self.candidate,
            journal,
            model=self.model,
            sleep=lambda _: None,
        )
        error = None
        try:
            client.run(ready_payload=b"\x99", kernel_sha256=KERNEL_SHA256)
        except (ProgrammingFailure, ValueError, TimeoutError) as e:
            error = e
        records = [json.loads(s) for s in journal.read_text().splitlines()]
        self.assertEqual(records[-1]["result"]["complete"], error is None)
        self.assertFalse(any(p[0] in (0x10, 0x11, 0x27, 0xA5) for p in peer.requests))
        return peer, client, error

    def test_plan_calibration_block_and_golden_packets(self):
        plan = make_plan(self.candidate, self.model)
        self.assertEqual(plan["changed_blocks"], [6])
        peer, client, error = self.run_case()
        self.assertIsNone(error)
        self.assertEqual(peer.image, self.candidate)
        self.assertEqual(client.report["blocks_verified"], [6])
        self.assertIn(bytes.fromhex("3101ff0106"), peer.requests)
        self.assertIn(bytes.fromhex("3400440004000000020000"), peer.requests)
        self.assertIn(bytes.fromhex("3500440004000000020000"), peer.requests)

    def test_busy_timeout_duplicate_and_sequence_wrap(self):
        peer, client, error = self.run_case("busy_and_drop", maximum=258)
        self.assertIsNone(error)
        self.assertTrue(peer.did_busy and peer.did_drop)
        self.assertEqual(peer.accepted_chunks, 512)
        self.assertEqual(peer.image, self.candidate)
        packets = [p for p in peer.requests if p[0] == 0x36 and len(p) > 2]
        self.assertEqual(packets[:3], [packets[0]] * 3)
        self.assertTrue(any(p[1] == 0 for p in packets))

    def test_unchanged_image_no_erase(self):
        peer, _, error = self.run_case(candidate=self.model.original)
        self.assertIsNone(error)
        self.assertEqual(peer.requests, [bytes.fromhex("3101ff00")] * 2)

    def test_matching_target_skips_erase_but_reads_back(self):
        peer, _, error = self.run_case(initial=self.candidate)
        self.assertIsNone(error)
        self.assertFalse(any(p[:4] == bytes.fromhex("3101ff01") for p in peer.requests))
        self.assertTrue(any(p[0] == 0x35 for p in peer.requests))

    def test_unknown_image_or_bad_inventory_stops_before_erase(self):
        for fault in ("inventory_map", None):
            initial = bytearray(self.model.original)
            if fault is None:
                initial[0x50000] ^= 1
            peer, client, error = self.run_case(fault, initial=initial)
            self.assertIsNotNone(error)
            self.assertFalse(client.report["erase_may_have_started"])
            self.assertFalse(any(p[0] == 0x34 for p in peer.requests))

    def test_erase_failure_and_timeout_never_download(self):
        for fault in ("erase_failure", "erase_timeout"):
            peer, client, error = self.run_case(fault)
            self.assertIsNotNone(error)
            self.assertTrue(client.report["erase_may_have_started"])
            self.assertFalse(any(p[0] == 0x34 for p in peer.requests))

    def test_write_failure_wrong_sequence_and_missing_terminal_ack(self):
        for fault in ("write_failure", "wrong_sequence", "missing_77"):
            peer, client, error = self.run_case(fault)
            self.assertIsNotNone(error)
            self.assertTrue(client.report["erase_may_have_started"])
            self.assertFalse(any(p[0] == 0x35 for p in peer.requests))

    def test_readback_and_final_inventory_must_both_match(self):
        for fault in ("readback_mismatch", "final_hash"):
            _, client, error = self.run_case(fault)
            self.assertIsNotNone(error)
            self.assertFalse(client.report["complete"])

    def test_ready_gate_no_commands(self):
        peer = KernelModel(self.model.original)
        client = KernelProgrammer(
            peer, self.candidate, Path("/unused"), model=self.model
        )
        for ready, sha in ((b"\x50", KERNEL_SHA256), (b"\x99", "wrong")):
            with self.assertRaises(ProgrammingFailure):
                client.run(ready_payload=ready, kernel_sha256=sha)
        self.assertEqual(peer.requests, [])

    def test_deadline_exhaustion(self):
        peer = KernelModel(self.model.original)
        with tempfile.TemporaryDirectory() as tmp:
            times = iter((0, 241))
            client = KernelProgrammer(
                peer,
                self.candidate,
                Path(tmp) / "journal",
                model=self.model,
                clock=lambda: next(times),
            )
            with self.assertRaises(TimeoutError):
                client.run(ready_payload=b"\x99", kernel_sha256=KERNEL_SHA256)
            self.assertFalse(client.report["complete"])
            self.assertEqual(peer.requests, [])


if __name__ == "__main__":
    unittest.main()
