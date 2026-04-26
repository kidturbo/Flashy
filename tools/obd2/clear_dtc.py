#!/usr/bin/env python3
"""clear_dtc.py — Universal OBD-II Clear DTC across every module on the bus.

Drives the Flashy firmware's CLEARDTC command, which:
  Pass 1: broadcasts OBD-II Mode $04 to functional ID 0x7DF and collects
          single-frame responses from any 0x7E8..0x7EF slot. Each ECU on
          the bus answers individually with $44 (cleared) or $7F 04 NRC.
  Pass 2: for each Pass-1 responder, sends physical UDS $10 03
          (extendedDiagnosticSession) + $14 FF FF FF
          (clearDiagnosticInformation, all groups) to its physical TX.

Works across manufacturers because Pass 1 is the OBD-II / SAE J1979
mandate and Pass 2 is the ISO 14229 (UDS) standard. GM modules in
default session may reject Pass 2 with NRC $11 — that's a known
manufacturer quirk, not a tool bug.

Usage:
    python tools/obd2/clear_dtc.py --port COM13
    python tools/obd2/clear_dtc.py --port COM13 --baud 500000

Exit code:
    0 = at least one module reported "cleared"
    1 = no module accepted either request

Requires: pip install pyserial
"""
from __future__ import annotations

import argparse
import re
import sys
import time

import serial


NRC_TABLE = {
    0x10: "generalReject",
    0x11: "serviceNotSupported",
    0x12: "subFunctionNotSupported",
    0x13: "incorrectMessageLengthOrInvalidFormat",
    0x21: "busyRepeatRequest",
    0x22: "conditionsNotCorrect",
    0x24: "requestSequenceError",
    0x31: "requestOutOfRange",
    0x33: "securityAccessDenied",
    0x35: "invalidKey",
    0x37: "requiredTimeDelayNotExpired",
    0x70: "uploadDownloadNotAccepted",
    0x72: "generalProgrammingFailure",
    0x78: "responsePending",
    0x7E: "subFunctionNotSupportedInActiveSession",
    0x7F: "serviceNotSupportedInActiveSession",
}


def nrc_name(byte: int) -> str:
    return NRC_TABLE.get(byte, f"unknown")


def drive(port: str, baud: int) -> int:
    s = serial.Serial(port, 115200, timeout=0.2)
    try:
        # Flush boot banner
        time.sleep(0.5)
        s.reset_input_buffer()

        def send_and_collect(cmd: str, settle: float) -> str:
            s.write((cmd + "\n").encode())
            s.flush()
            buf = b""
            t_end = time.monotonic() + settle
            while time.monotonic() < t_end:
                chunk = s.read(4096)
                if chunk:
                    buf += chunk
            return buf.decode("utf-8", errors="replace")

        # Initialize CAN at the requested baud
        init_out = send_and_collect(f"INIT {baud}", 1.5)
        if "CAN initialized" not in init_out:
            print(f"INIT failed:\n{init_out}", file=sys.stderr)
            return 2
        print(f"[INIT {baud} OK]")

        # Drive CLEARDTC
        out = send_and_collect("CLEARDTC", 6.0)
        sys.stdout.write(out)
        sys.stdout.flush()

        # Parse outcome from firmware output
        if re.search(r"^OK\s*$", out, re.MULTILINE):
            return 0
        m = re.search(r"Pass 2 summary: cleared=(\d+)", out)
        if m and int(m.group(1)) > 0:
            return 0
        if re.search(r"cleared=([1-9]\d*)\s+nrc=0", out):
            return 0
        return 1

    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", required=True, help="serial port (e.g. COM13 or /dev/ttyACM0)")
    ap.add_argument("--baud", type=int, default=500000, help="CAN baud (default 500000)")
    args = ap.parse_args()

    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(drive(args.port, args.baud))


if __name__ == "__main__":
    main()
