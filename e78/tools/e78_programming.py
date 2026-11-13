# SPDX-License-Identifier: GPL-3.0-or-later
"""E78 calibration block planner and injected-transport programming core.

Protocol: pinned KernelMPC5566 Version1.1 / 320adc3. No live launcher, adapter,
unlock, loader, reset or automatic recovery is supplied. Tested offline only.
"""
import argparse
import json
import os
from pathlib import Path
import time

from e78_candidate import CalibrationImage, digest, EXPECTED
from e78_read_protocol import UploadDecoder, KERNEL_SHA256

BLOCKS = (
    (0, 0x4000),
    (0x4000, 0xC000),
    (0x10000, 0xC000),
    (0x1C000, 0x4000),
    (0x20000, 0x10000),
    (0x30000, 0x10000),
    (0x40000, 0x20000),
    (0x60000, 0x20000),
) + tuple((a, 0x20000) for a in range(0x80000, 0x300000, 0x20000))
CAL_BLOCKS = frozenset((4, 5, 6, 7))


def make_plan(candidate, model=None):
    model = model or CalibrationImage()
    if not all(r["passed"] for r in model.verify(candidate)):
        raise ValueError("Candidate checksum failure; correct offline first")
    blocks = []
    for ident, (address, size) in enumerate(BLOCKS):
        old = model.original[address : address + size]
        new = candidate[address : address + size]
        changed = old != new
        if changed and ident not in CAL_BLOCKS:
            raise ValueError("Changes outside calibration blocks prohibited")
        blocks.append(
            dict(
                id=ident,
                address=address,
                bytes=size,
                original_sha256=digest(old),
                target_sha256=digest(new),
                changed=changed,
            )
        )
    return dict(
        original_sha256=EXPECTED,
        candidate_sha256=digest(candidate),
        kernel_sha256=KERNEL_SHA256,
        blocks=blocks,
        changed_blocks=[b["id"] for b in blocks if b["changed"]],
        flash_ready=False,
        hardware_validated=False,
        scope="Exact-image offline plan; full physical blocks, calibration area only",
    )


class ProgrammingFailure(RuntimeError):
    pass


class KernelProgrammer:
    """State machine for a freshly loaded, identity-checked pinned kernel.

    A caller must separately establish live authorization, identity, power and
    recovery. This repository supplies only synthetic callers. Once any erase
    may have been sent, failure must preserve power and require manual recovery
    assessment; this class never sends reset, exits the kernel or auto-resumes.
    """

    def __init__(
        self,
        transport,
        candidate,
        journal,
        *,
        model=None,
        budget=240,
        retries=3,
        clock=time.monotonic,
        sleep=time.sleep,
    ):
        if not 0 < budget <= 600 or not 0 <= retries <= 10:
            raise ValueError("Invalid bounded budget/retries")
        self.model = model or CalibrationImage()
        self.candidate = bytes(candidate)
        self.plan = make_plan(self.candidate, self.model)
        self.transport = transport
        self.journal = Path(journal)
        self.budget = budget
        self.retries = retries
        self.clock, self.sleep = clock, sleep
        self.stage = "preflight"
        self.deadline = None
        self.stream = None
        self.report = dict(
            complete=False,
            hardware_validated=False,
            blocks_verified=[],
            erase_may_have_started=False,
            candidate_sha256=digest(self.candidate),
        )

    def event(self, **event):
        record = dict(stage=self.stage, **event)
        self.stream.write(json.dumps(record) + "\n")
        self.stream.flush()
        os.fsync(self.stream.fileno())

    def remaining(self, cap=5):
        left = self.deadline - self.clock()
        if left <= 0:
            raise TimeoutError("Overall programming budget exhausted")
        return min(left, cap)

    def send(self, payload):
        # Journal intent before any transport call; payload data is represented by hash.
        self.event(
            request=payload[:6].hex(), length=len(payload), sha256=digest(payload)
        )
        self.transport.send(payload, timeout=self.remaining())

    def receive(self, service, limit):
        while True:
            left = min(self.remaining(), limit - self.clock())
            if left <= 0:
                raise TimeoutError("Response deadline exhausted")
            reply = self.transport.recv(timeout=left)
            if reply is None:
                raise TimeoutError("No response")
            reply = bytes(reply)
            self.event(
                response=reply[:8].hex(), length=len(reply), sha256=digest(reply)
            )
            if reply[:1] == b"\x7f":
                if len(reply) != 3 or reply[1] != service:
                    raise ProgrammingFailure(
                        "Malformed or mismatched negative response"
                    )
                if reply[2] == 0x78:
                    continue
            return reply

    def exchange(self, payload, *, retry_busy=False, retry_timeout=False):
        for attempt in range(self.retries + 1):
            limit = self.clock() + self.remaining()
            try:
                self.send(payload)
                reply = self.receive(payload[0], limit)
            except TimeoutError:
                if not retry_timeout or attempt == self.retries:
                    raise
                continue
            if reply[:1] != b"\x7f":
                return reply
            if (
                reply == bytes((0x7F, payload[0], 0x21))
                and retry_busy
                and attempt < self.retries
            ):
                self.sleep(min(0.02, self.remaining()))
                continue
            raise ProgrammingFailure(f"Negative response {reply.hex()}")
        raise ProgrammingFailure("Retry budget exhausted")

    def inventory(self):
        self.send(bytes.fromhex("3101ff00"))
        result = []
        limit = self.clock() + self.remaining(45)
        for expected_id, (address, size) in enumerate(BLOCKS):
            reply = self.receive(0x31, limit)
            if (
                len(reply) != 46
                or reply[:5] != bytes.fromhex("7101ff001c")
                or reply[5] != expected_id
                or int.from_bytes(reply[6:10], "big") != address
                or int.from_bytes(reply[10:14], "big") != size
            ):
                raise ProgrammingFailure(
                    "Malformed/out-of-order/mismatched block inventory"
                )
            result.append(reply[14:].hex())
        return result

    def erase(self, ident):
        if ident not in CAL_BLOCKS or ident not in self.plan["changed_blocks"]:
            raise ProgrammingFailure("Erase outside planned calibration blocks")
        self.stage = "erase"
        self.report["erase_may_have_started"] = True
        self.send(bytes.fromhex("3101ff01") + bytes((ident,)))
        limit = self.clock() + self.remaining(45)
        previous = -1
        for _ in range(16):
            reply = self.receive(0x31, limit)
            if len(reply) != 6 or reply[:5] != bytes.fromhex("7101ff01") + bytes(
                (ident,)
            ):
                raise ProgrammingFailure("Malformed erase status")
            status = reply[5]
            if status == 3:
                raise ProgrammingFailure("Erase failed")
            if status not in (0, 1, 2) or status < previous:
                raise ProgrammingFailure("Invalid erase status progression")
            if status == 2:
                self.event(erase_terminal_success=ident)
                return
            previous = status
        raise ProgrammingFailure("Too many nonterminal erase statuses")

    def download(self, ident):
        self.stage = "download"
        address, size = BLOCKS[ident]
        data = self.candidate[address : address + size]
        request = b"\x34\x00\x44" + address.to_bytes(4, "big") + size.to_bytes(4, "big")
        reply = self.exchange(request)
        if len(reply) != 4 or reply[:2] != b"\x74\x20":
            raise ProgrammingFailure("Bad RequestDownload acknowledgement")
        maximum = int.from_bytes(reply[2:], "big")
        if not 10 <= maximum <= 4095:
            raise ProgrammingFailure("Invalid negotiated message length")
        chunk_size = ((maximum - 2) // 8) * 8
        sequence = 1
        for offset in range(0, size, chunk_size):
            chunk = data[offset : offset + chunk_size]
            if len(chunk) % 8:
                raise ProgrammingFailure("Unaligned internal chunk")
            packet = bytes((0x36, sequence)) + chunk
            reply = self.exchange(packet, retry_busy=True, retry_timeout=True)
            if reply != bytes((0x76, sequence)):
                raise ProgrammingFailure("TransferData sequence/length mismatch")
            sequence = (sequence + 1) & 255
        self.stage = "transfer-exit"
        # Busy is retryable; missing 77 is ambiguous, so timeout stops the session.
        if self.exchange(b"\x37", retry_busy=True) != b"\x77":
            raise ProgrammingFailure("Missing terminal TransferExit 77")
        self.event(transfer_complete=ident)

    def readback(self, ident):
        self.stage = "readback"
        address, size = BLOCKS[ident]
        request = b"\x35\x00\x44" + address.to_bytes(4, "big") + size.to_bytes(4, "big")
        decoder = UploadDecoder(self.exchange(request), size=size)
        while len(decoder.data) < size:
            decoder.accept(
                self.exchange(
                    decoder.next_request(), retry_busy=True, retry_timeout=True
                )
            )
        data = decoder.confirm_exit(self.exchange(decoder.exit_request()))
        if data != self.candidate[address : address + size]:
            raise ProgrammingFailure("Full block readback differs from candidate")
        self.event(readback_verified=ident, sha256=digest(data))

    def run(self, *, ready_payload, kernel_sha256):
        if ready_payload != b"\x99" or kernel_sha256 != KERNEL_SHA256:
            raise ProgrammingFailure("Pinned kernel/ready gate failed")
        self.deadline = self.clock() + self.budget
        with self.journal.open("x") as self.stream:
            try:
                self.event(
                    plan=self.plan,
                    validation="offline-tested core; caller-supplied readiness is not live proof",
                )
                self.stage = "inventory-before"
                inventory = self.inventory()
                for block, reported in zip(self.plan["blocks"], inventory):
                    allowed = {block["original_sha256"], block["target_sha256"]}
                    if reported not in allowed:
                        raise ProgrammingFailure(
                            "Unknown block contents; no erase authorized by this plan"
                        )
                for ident in self.plan["changed_blocks"]:
                    target = self.plan["blocks"][ident]["target_sha256"]
                    if inventory[ident] != target:
                        self.erase(ident)
                        self.download(ident)
                    else:
                        self.event(skipped_matching_block=ident)
                    self.readback(ident)
                    self.report["blocks_verified"].append(ident)
                self.stage = "inventory-after"
                final = self.inventory()
                if final != [b["target_sha256"] for b in self.plan["blocks"]]:
                    raise ProgrammingFailure(
                        "Final full inventory differs from candidate"
                    )
                self.report["complete"] = True
            except BaseException as error:
                self.report["error"] = f"{type(error).__name__}: {error}"
                raise
            finally:
                self.report["last_stage"] = self.stage
                self.report["power_action"] = (
                    "No reset/exit/power-control command sent; assess prepared restoration/recovery separately."
                )
                self.event(result=self.report)
        return self.report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Offline plan only; never connects a vehicle"
    )
    parser.add_argument("candidate", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    plan = make_plan(args.candidate.read_bytes())
    with args.output.open("x") as f:
        json.dump(plan, f, indent=2)
        f.write("\n")
    print(json.dumps(dict(changed_blocks=plan["changed_blocks"], flash_ready=False)))
