# SPDX-License-Identifier: GPL-3.0-or-later
"""E78 original-read state machine, with an injected ISO-TP transport.

Stock protocol/key derivation follows FL0WL0W/ESP32-ECU-Flasher aa60be5;
the retained GPL license is in vendor/ESP32-ECU-Flasher-LICENSE.
No erase or persistent flash-write operation is implemented. Loading the
pinned kernel writes SRAM and leaves the ECU in the kernel until an externally
verified restoration procedure is performed. No hardware transport or live launcher is supplied by this package.
"""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import time

from e78_read_protocol import (
    verified_ram_packets,
    upload_request,
    UploadDecoder,
    MAIN_FLASH_BYTES,
    ECC_SUBSTITUTIONS,
    KERNEL_SHA256,
)
from e78_identity import decode_result


class ReadFailure(RuntimeError):
    pass


class NegativeResponse(ReadFailure):
    def __init__(self, response):
        self.response = bytes(response)
        self.code = self.response[2]
        super().__init__(f"ECU negative response {self.response.hex()}")


def key_for_two_byte_seed(seed):
    """Published two-byte algorithm used by the originating exact-profile read."""
    if len(seed) != 2:
        raise ReadFailure("Only the source-documented two-byte E78 seed is supported")
    key = (int.from_bytes(seed, "little") + 0x0801) & 0xFFFF
    key = ((key >> 8) | (key << 8)) & 0xFFFF
    key = (key + 0x5063) & 0xFFFF
    key = ((key >> 1) | (key << 15)) & 0xFFFF
    return key.to_bytes(2, "big")


class E78ReadWorkflow:
    def __init__(
        self,
        transport,
        kernel,
        identity,
        output,
        *,
        compressed=True,
        budget_seconds=300,
        progress=None,
    ):
        self.transport = (
            transport  # send(payload, timeout), recv(timeout): bytes or TimeoutError
        )
        self.packets = verified_ram_packets(kernel)
        self.identity = identity
        self.expected_vin = identity["reads"]["0902"]["vin"]
        self.expected_calids = identity["reads"]["0904"]["calibration_ids"]
        self.expected_gm_id = identity["reads"]["1ac9"]["gm_1ac9_identifier"]
        if (
            len(self.expected_vin) != 17
            or not self.expected_calids
            or self.expected_calids[0] != "12644082"
        ):
            raise ReadFailure("This workflow is scoped to the recorded ECU identity")
        if not 0 < budget_seconds <= 600:
            raise ValueError("A bounded session budget up to 600 seconds is required")
        self.compressed = compressed
        self.budget_seconds = budget_seconds
        self.progress = progress
        self.deadline = None
        self.output = Path(output)
        self.stage = "identity"
        self.ram_index = 0
        self.allowed_key = None
        self.decoder = None
        self.report = {
            "started_utc": datetime.now(timezone.utc).isoformat(),
            "complete": False,
            "kernel_sha256": KERNEL_SHA256,
            "compressed": compressed,
            "main_flash_bytes": MAIN_FLASH_BYTES,
            "shadow_included": False,
            "persistent_flash_write_commands": 0,
            "events": [],
            "reads": [],
            "restoration_required": False,
            "stock_operation_restored": False,
            "normal_communication_disabled": False,
            "ram_download_attempted": False,
            "kernel_execution_attempted": False,
        }

    def notify(self, **details):
        if self.progress is not None:
            self.progress({"stage": self.stage, **details})

    def guard(self, payload):
        allowed = False
        if self.stage == "identity":
            allowed = payload in (b"\x09\x02", b"\x09\x04", b"\x1a\xc9")
        elif self.stage == "unlock":
            allowed = payload == b"\x27\x01" or (
                self.allowed_key is not None
                and payload == b"\x27\x02" + self.allowed_key
            )
        elif self.stage == "quiet":
            allowed = payload == b"\x28"
        elif self.stage == "program":
            allowed = payload in (b"\x10\x02", b"\xa5\x01", b"\xa5\x03")
        elif self.stage == "ram":
            allowed = (
                self.ram_index < len(self.packets) - 1
                and payload == self.packets[self.ram_index]
            )
        elif self.stage == "execute":
            allowed = (
                self.ram_index == len(self.packets) - 1 and payload == self.packets[-1]
            )
        elif self.stage == "read":
            if payload == upload_request(self.compressed) and (
                self.decoder is None or self.decoder.finished
            ):
                allowed = True
            elif self.decoder is not None:
                if len(self.decoder.data) < self.decoder.size:
                    allowed = payload == self.decoder.next_request()
                else:
                    allowed = payload == self.decoder.exit_request()
        if not allowed:
            raise ReadFailure(
                f"Payload {payload[:8].hex()} is not allowed in stage {self.stage}"
            )

    def exchange(
        self, payload, positive, *, timeout=3, optional_timeout=False, ignore=()
    ):
        self.guard(payload)
        now = time.monotonic()
        if now >= self.deadline:
            raise TimeoutError("Overall read-session time limit reached")
        limit = min(self.deadline, now + timeout)
        self.report["events"].append(
            {
                "stage": self.stage,
                "service": f"{payload[0]:02x}",
                "request_length": len(payload),
            }
        )
        self.transport.send(payload, timeout=max(0.001, limit - now))
        while time.monotonic() < limit:
            try:
                response = self.transport.recv(
                    timeout=max(0.001, limit - time.monotonic())
                )
            except TimeoutError:
                break
            if response is None:
                break
            response = bytes(response)
            if response[:2] == b"\x7f" + payload[:1]:
                if len(response) != 3:
                    raise ReadFailure("Malformed negative response")
                if response[2] == 0x78:
                    continue
                raise NegativeResponse(response)
            if any(response.startswith(prefix) for prefix in ignore):
                continue
            if not response.startswith(positive):
                raise ReadFailure(
                    f"Unexpected response to {payload[0]:02x}: {response[:8].hex()}"
                )
            return response
        if optional_timeout and time.monotonic() < self.deadline:
            self.report["events"][-1]["timeout_allowed_by_upstream_protocol"] = True
            return None
        raise TimeoutError(f"No expected reply to service {payload[0]:02x}")

    def identify_and_unlock(self):
        for request, key, expected in (
            (b"\x09\x02", "vin", self.expected_vin),
            (b"\x09\x04", "calibration_ids", self.expected_calids),
            (b"\x1a\xc9", "gm_1ac9_identifier", self.expected_gm_id),
        ):
            reply = self.exchange(request, bytes((request[0] + 0x40, request[1])))
            if decode_result(request, reply).get(key) != expected:
                raise ReadFailure(
                    f"Live ECU identity mismatch for {key}; no unlock attempted"
                )
        self.stage = "unlock"
        self.notify()
        response = self.exchange(b"\x27\x01", b"\x67\x01")
        if len(response) != 4:
            raise ReadFailure("Unsupported seed format; no key attempt made")
        seed = response[2:]
        if seed != bytes(2):
            self.allowed_key = key_for_two_byte_seed(seed)
            reply = self.exchange(b"\x27\x02" + self.allowed_key, b"\x67\x02")
            if reply != b"\x67\x02":
                raise ReadFailure("Unexpected security-key acknowledgement")
        self.report["stock_security_accepted"] = True

    def load_kernel(self):
        self.stage = "quiet"
        self.notify()
        self.exchange(b"\x28", b"\x68")
        self.report["normal_communication_disabled"] = True
        self.report["restoration_required"] = True
        self.stage = "program"
        self.notify()
        self.exchange(b"\x10\x02", b"\x50")
        self.exchange(b"\xa5\x01", b"\xe5", timeout=1, optional_timeout=True)
        self.exchange(b"\xa5\x03", b"\xe5", timeout=1, optional_timeout=True)
        self.stage = "ram"
        self.notify(kernel_bytes=30704)
        self.report["ram_download_attempted"] = True
        for index, payload in enumerate(self.packets[:-1]):
            self.ram_index = index
            self.exchange(payload, b"\x74" if index == 0 else b"\x76", timeout=5)
        self.ram_index = len(self.packets) - 1
        self.stage = "execute"
        self.notify()
        self.report["kernel_execution_attempted"] = True
        ready = self.exchange(self.packets[-1], b"\x99", timeout=5, ignore=(b"\x76",))
        if ready != b"\x99":
            raise ReadFailure("Malformed kernel-ready response")
        self.report["kernel_ready_received"] = True
        self.stage = "read"
        self.notify(kernel_ready=True)

    def read_one(self, number):
        self.notify(pass_number=number, bytes_received=0)
        header = self.exchange(upload_request(self.compressed), b"\x75")
        self.decoder = UploadDecoder(header, compressed=self.compressed)
        next_notice = 65536
        try:
            while len(self.decoder.data) < MAIN_FLASH_BYTES:
                request = self.decoder.next_request()
                for attempt in range(2):
                    try:
                        reply = self.exchange(request, b"\x76", timeout=5)
                        break
                    except TimeoutError:
                        if attempt == 1:
                            raise
                    except NegativeResponse as error:
                        if error.code != 0x21 or attempt == 1:
                            raise
                self.decoder.accept(reply)
                if len(self.decoder.data) >= next_notice:
                    self.notify(
                        pass_number=number,
                        bytes_received=len(self.decoder.data),
                        total_bytes=MAIN_FLASH_BYTES,
                    )
                    next_notice += 65536
            reply = self.exchange(self.decoder.exit_request(), b"\x77")
            data = self.decoder.confirm_exit(reply)
            if len(data) != MAIN_FLASH_BYTES or len(set(data)) < 2:
                raise ReadFailure(
                    "Full image is missing or uniform; not accepted as a dump"
                )
            for start, end in ECC_SUBSTITUTIONS:
                if data[start:end] != b"\xff" * (end - start):
                    raise ReadFailure(
                        "Image does not match the pinned kernel ECC substitutions"
                    )
            filename = f"pass-{number}.bin"
            with (self.output / filename).open("xb") as output:
                output.write(data)
                output.flush()
                os.fsync(output.fileno())
            digest = hashlib.sha256(data).hexdigest()
            self.report["reads"].append(
                {"file": filename, "bytes": len(data), "sha256": digest}
            )
            return digest
        except BaseException:
            if self.decoder.data:
                with (self.output / f"pass-{number}.partial.bin").open("xb") as output:
                    output.write(self.decoder.data)
                    output.flush()
                    os.fsync(output.fileno())
            raise

    def run(self):
        self.output.mkdir(mode=0o700)
        self.deadline = time.monotonic() + self.budget_seconds
        self.report["budget_seconds"] = self.budget_seconds
        try:
            self.notify()
            self.identify_and_unlock()
            self.load_kernel()
            first = self.read_one(1)
            second = self.read_one(2)
            if first != second:
                raise ReadFailure("Independent full reads do not match")
            self.report["complete"] = True
            self.report["verified_sha256"] = first
        except BaseException as error:
            self.report["error"] = f"{type(error).__name__}: {error}"
        finally:
            self.report["last_stage"] = self.stage
            self.report["ended_utc"] = datetime.now(timezone.utc).isoformat()
            self.report["power_cycle_may_be_required"] = self.report[
                "ram_download_attempted"
            ]
            self.report["restoration_note"] = (
                "Check stock operation after ignition cycle; full ECU power cycle if still in kernel/bootloader"
                if self.report["ram_download_attempted"]
                else "Ignition OFF; verify stock operation on next RUN. No kernel uploaded."
            )
            with (self.output / "manifest.json").open("x") as output:
                json.dump(self.report, output, indent=2)
                output.write("\n")
                output.flush()
                os.fsync(output.fileno())
        return self.report
