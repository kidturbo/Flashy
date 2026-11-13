#!/usr/bin/env python3
"""Offline packet planning/decoding for the pinned E78 read path. NO hardware I/O.

Protocol references: FL0WL0W/ESP32-ECU-Flasher at aa60be5 and
FL0WL0W/KernelMPC5566 Version1.1 at 320adc3. This is not a live loader.
Stock unlock/session transitions and reliable ECU restoration still require
target-specific validation before an integrated client can be used.
"""
import argparse
import hashlib
import json
from pathlib import Path

KERNEL_SHA256 = "f694f67e639db1e4465797545c3cca4b00883bd5f068ae2a3bf3dcd08d97c861"
KERNEL_BYTES = 30704
RAM_ENTRY = 0x40014000
RAM_LIMIT = 0x40020000
MAIN_FLASH_BYTES = 0x300000
ECC_SUBSTITUTIONS = ((0x3FE0, 0x4000), (0x1FFE0, 0x20000))  # End-exclusive.


def _ram_packets(image):
    """Source-derived stock format; only addresses in this kernel's RAM area."""
    if not 0 < len(image) <= RAM_LIMIT - RAM_ENTRY:
        raise ValueError("Kernel does not fit the fixed RAM code region")
    chunks = [
        (offset, image[offset : offset + 2042]) for offset in range(0, len(image), 2042)
    ]
    encoded_length = len(image) + 6 * len(chunks)
    packets = [b"\x34\x00" + encoded_length.to_bytes(3, "big")]
    packets.extend(
        b"\x36\x00" + (RAM_ENTRY + offset).to_bytes(4, "big") + data
        for offset, data in reversed(chunks)
    )
    packets.append(b"\x36\x80" + RAM_ENTRY.to_bytes(4, "big"))
    return packets


def verified_ram_packets(image):
    if len(image) != KERNEL_BYTES or hashlib.sha256(image).hexdigest() != KERNEL_SHA256:
        raise ValueError("Image differs from the pinned, published kernel asset")
    return _ram_packets(image)


def upload_request(compressed=False):
    """Only after verified kernel-ready 99: complete main-flash read."""
    return (
        bytes((0x35, 0x10 if compressed else 0, 0x44))
        + bytes(4)
        + MAIN_FLASH_BYTES.to_bytes(4, "big")
    )


class UploadDecoder:
    """Raw or independent LZ4 kernel blocks; emits only read-transfer requests."""

    def __init__(self, reply, size=MAIN_FLASH_BYTES, compressed=False):
        if len(reply) != 4 or reply[:2] != b"\x75\x20":
            raise ValueError("Unexpected RequestUpload response")
        self.max_message = int.from_bytes(reply[2:], "big")
        if not 3 <= self.max_message <= 4095 or not 0 < size <= MAIN_FLASH_BYTES:
            raise ValueError("Invalid message limit or read size")
        self.size = size
        self.sequence = 1
        self.data = bytearray()
        self.finished = False
        self.compressed = compressed
        if compressed:
            import lz4.block

            self.decompress = lz4.block.decompress

    def next_request(self):
        if len(self.data) == self.size:
            raise ValueError("All requested bytes are present; use TransferExit")
        # Calling this again after a timeout preserves the same sequence.
        return bytes((0x36, self.sequence))

    def accept(self, response):
        if len(self.data) == self.size:
            raise ValueError("Unexpected data after requested range")
        if (
            response[:2] != bytes((0x76, self.sequence))
            or len(response) > self.max_message
        ):
            raise ValueError("Upload response sequence or size mismatch")
        if self.compressed:
            if len(response) < 5:
                raise ValueError("Truncated compressed upload response")
            decoded_length = int.from_bytes(response[2:4], "big")
            if not 0 < decoded_length <= min(4096, self.size - len(self.data)):
                raise ValueError("Invalid decoded block length")
            data = self.decompress(response[4:], uncompressed_size=decoded_length)
            if len(data) != decoded_length:
                raise ValueError("Decoded length does not match header")
        else:
            expected = min(self.max_message - 2, self.size - len(self.data))
            if len(response) != expected + 2:
                raise ValueError("Upload response size mismatch")
            data = response[2:]
        self.data.extend(data)
        self.sequence = (self.sequence + 1) & 0xFF

    def exit_request(self):
        if len(self.data) != self.size:
            raise ValueError("Incomplete read cannot be finalized")
        return b"\x37"

    def confirm_exit(self, response):
        self.exit_request()
        if response != b"\x77":
            raise ValueError("Missing final TransferExit acknowledgement")
        self.finished = True
        return bytes(self.data)


def plan(image):
    packets = verified_ram_packets(image)
    return {
        "offline_only": True,
        "hardware_commands_sent": 0,
        "kernel_sha256": KERNEL_SHA256,
        "kernel_bytes": len(image),
        "source_tag": "Version1.1",
        "source_commit": "320adc3a18985aff5614f2cd2ce5fc40ac001456",
        "stock_phase": {
            "prerequisites": [
                "Matched ECU identity",
                "Stable power",
                "Verified stock unlock/session path",
                "Verified way to restore stock operation; ignition OFF alone is not assumed sufficient",
            ],
            "request_download_payload": packets[0].hex(),
            "chunks": [
                {
                    "destination": f'{int.from_bytes(p[2:6], "big"):08X}',
                    "data_bytes": len(p) - 6,
                    "data_sha256": hashlib.sha256(p[6:]).hexdigest(),
                }
                for p in packets[1:-1]
            ],
            "execute_payload": packets[-1].hex(),
            "required_ready_payload": "99",
        },
        "kernel_read_phase": {
            "request_upload_payload": upload_request().hex(),
            "range_start": "00000000",
            "range_bytes": MAIN_FLASH_BYTES,
            "sequence_start": 1,
            "sequence_wrap": 0,
            "transfer_exit": "37",
            "required_exit_response": "77",
            "ecc_substitutions_end_exclusive": [
                [f"{a:08X}", f"{b:08X}"] for a, b in ECC_SUBSTITUTIONS
            ],
            "shadow_included": False,
            "repeat_matching_read_required": True,
        },
        "limits": [
            "No live transport or stock security implemented here",
            "No erase, persistent flash write or ECU reset implementation",
            "Packet tests are not physical loader validation",
        ],
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(plan(args.kernel.read_bytes()), indent=2))
