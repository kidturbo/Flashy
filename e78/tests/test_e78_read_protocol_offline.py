"""Pure byte-array tests. No serial/CAN devices or ECU access."""

import hashlib
import unittest

try:
    import lz4.block
except ModuleNotFoundError:
    lz4 = None
from e78_read_protocol import (
    _ram_packets,
    verified_ram_packets,
    UploadDecoder,
    RAM_ENTRY,
    RAM_LIMIT,
    MAIN_FLASH_BYTES,
    ECC_SUBSTITUTIONS,
    upload_request,
)


class ProtocolTests(unittest.TestCase):
    def test_stock_ram_chunks_cover_input_once_in_descending_order(self):
        for size in (1, 2042, 2043, 30704, RAM_LIMIT - RAM_ENTRY):
            image = (bytes(range(256)) * (size // 256 + 1))[:size]
            packets = _ram_packets(image)
            chunks = packets[1:-1]
            self.assertEqual(
                int.from_bytes(packets[0][2:], "big"), size + 6 * len(chunks)
            )
            addresses = [int.from_bytes(p[2:6], "big") for p in chunks]
            self.assertEqual(addresses, sorted(addresses, reverse=True))
            self.assertTrue(all(RAM_ENTRY <= a < RAM_LIMIT for a in addresses))
            self.assertEqual(b"".join(p[6:] for p in reversed(chunks)), image)
            self.assertTrue(
                all(len(p) <= 2048 and p[:2] == b"\x36\x00" for p in chunks)
            )
            self.assertEqual(packets[-1], bytes.fromhex("368040014000"))

    def test_wrong_binary_and_bad_sizes_rejected(self):
        with self.assertRaises(ValueError):
            verified_ram_packets(bytes(30704))
        for image in (b"", bytes(RAM_LIMIT - RAM_ENTRY + 1)):
            with self.assertRaises(ValueError):
                _ram_packets(image)

    def test_complete_main_flash_sequence_wrap_retry_and_exit(self):
        expected = bytearray(bytes(range(256)) * (MAIN_FLASH_BYTES // 256))
        for start, end in ECC_SUBSTITUTIONS:
            expected[start:end] = b"\xff" * (end - start)
        receiver = UploadDecoder(bytes.fromhex("75200fff"))
        self.assertEqual(upload_request(), bytes.fromhex("3500440000000000300000"))
        sequences = []
        while len(receiver.data) < len(expected):
            request = receiver.next_request()
            self.assertEqual(
                receiver.next_request(), request
            )  # Lost reply: retry same counter.
            sequences.append(request[1])
            offset = len(receiver.data)
            data = expected[offset : offset + 4093]
            receiver.accept(bytes((0x76, request[1])) + data)
        self.assertIn(0, sequences)
        self.assertEqual(len(sequences), 769)
        self.assertEqual(receiver.exit_request(), b"\x37")
        result = receiver.confirm_exit(b"\x77")
        self.assertEqual(
            hashlib.sha256(result).digest(), hashlib.sha256(expected).digest()
        )
        self.assertTrue(receiver.finished)

    def test_wrong_sequence_length_and_early_exit_do_not_advance(self):
        receiver = UploadDecoder(bytes.fromhex("75200fff"), size=16)
        for reply in (
            b"\x76\x02" + bytes(16),
            b"\x76\x01" + bytes(15),
            b"\x7f\x36\x73",
        ):
            with self.assertRaises(ValueError):
                receiver.accept(reply)
            self.assertEqual(len(receiver.data), 0)
            self.assertEqual(receiver.next_request(), b"\x36\x01")
        with self.assertRaises(ValueError):
            receiver.exit_request()
        receiver.accept(b"\x76\x01" + bytes(16))
        with self.assertRaises(ValueError):
            receiver.confirm_exit(b"\x7f\x37\x24")

    @unittest.skipIf(lz4 is None, "Optional lz4 dependency not installed")
    def test_compressed_blocks_accept_variable_decoded_size_and_strict_length(self):
        receiver = UploadDecoder(bytes.fromhex("75200fff"), size=6144, compressed=True)
        first = bytes(range(256)) * 16
        second = b"X" * 2048
        for sequence, block in ((1, first), (2, second)):
            receiver.accept(
                bytes((0x76, sequence))
                + len(block).to_bytes(2, "big")
                + lz4.block.compress(block, store_size=False)
            )
        self.assertEqual(receiver.confirm_exit(b"\x77"), first + second)
        self.assertEqual(upload_request(True), bytes.fromhex("3510440000000000300000"))

    @unittest.skipIf(lz4 is None, "Optional lz4 dependency not installed")
    def test_invalid_lz4_blocks_do_not_advance(self):
        good = lz4.block.compress(b"X" * 4096, store_size=False)
        replies = (
            b"\x76\x01",
            b"\x76\x01\x00\x00\x00",
            b"\x76\x01\x10\x01" + good,
            b"\x76\x01\x10\x00\xff",
            b"\x76\x01\x0f\xff" + good,
            b"\x76\x02\x10\x00" + good,
        )
        receiver = UploadDecoder(bytes.fromhex("75200fff"), compressed=True)
        for response in replies:
            with self.assertRaises((ValueError, lz4.block.LZ4BlockError)):
                receiver.accept(response)
            self.assertEqual(len(receiver.data), 0)
            self.assertEqual(receiver.next_request(), b"\x36\x01")


if __name__ == "__main__":
    unittest.main()
