#!/usr/bin/env python3
"""scan_full.py — OBD-II Mode 9 dump for every module on the bus.

Drives the Flashy firmware's SCAN FULL command, which probes the eight
GM diagnostic slots (0x7E0..0x7E7) and for every responder pulls:
  - VIN          (GM $1A 90, falls back to UDS $22 F190)
  - ECU Name     (Mode 9 PID 0A — 20 ASCII bytes)
  - CAL IDs      (Mode 9 PID 04 — list of 16-byte cal IDs, count varies)
  - CVNs         (Mode 9 PID 06 — 4-byte hashes, one per cal ID per
                  J1979 emissions verification mandate)
  - $1A B4 string (GM ReadDataByLocalIdentifier — alphanumeric cal info)

Output formats:
  default     — pass-through firmware text (matches what the serial
                console shows interactively)
  --json      — parsed structured JSON, one object per module

Usage:
    python tools/obd2/scan_full.py --port COM13
    python tools/obd2/scan_full.py --port COM13 --json > bench.json

Requires: pip install pyserial
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import time

import serial


def drive(port: str, baud: int) -> str:
    s = serial.Serial(port, 115200, timeout=0.2)
    try:
        time.sleep(0.5)
        s.reset_input_buffer()

        def send_and_collect(cmd: str, settle: float) -> str:
            s.write((cmd + "\n").encode())
            s.flush()
            buf = b""
            t_end = time.monotonic() + settle
            while time.monotonic() < t_end:
                chunk = s.read(8192)
                if chunk:
                    buf += chunk
            return buf.decode("utf-8", errors="replace")

        init_out = send_and_collect(f"INIT {baud}", 1.5)
        if "CAN initialized" not in init_out:
            raise SystemExit(f"INIT failed:\n{init_out}")

        # SCAN FULL probes 8 modules, may take 15-20 s on a busy bus.
        return send_and_collect("SCAN FULL", 25.0)
    finally:
        s.close()


def parse(text: str) -> list[dict]:
    """Split firmware SCAN FULL output into one dict per FOUND module."""
    modules: list[dict] = []
    cur: dict | None = None
    section: str | None = None  # "calids" | "cvns" | None

    for line in text.splitlines():
        m = re.match(r"^SCAN:FOUND\s+(0x[0-9A-Fa-f]+)/(0x[0-9A-Fa-f]+)\s+(\S+)", line)
        if m:
            if cur is not None:
                modules.append(cur)
            cur = {
                "tx_id":   m.group(1),
                "rx_id":   m.group(2),
                "slot":    m.group(3),
                "vin":     None,
                "ecu_name": None,
                "cal_ids": [],
                "cvns":    [],
                "lid_b4":  None,
            }
            section = None
            continue
        if cur is None:
            continue

        m = re.match(r"^\s+VIN:\s+(\S+)", line)
        if m:
            cur["vin"] = m.group(1)
            continue
        m = re.match(r"^\s+ECU Name:\s+(.+?)\s*$", line)
        if m:
            cur["ecu_name"] = m.group(1)
            continue
        m = re.match(r"^\s+CAL IDs\s*\(\d+\):", line)
        if m:
            section = "calids"
            continue
        m = re.match(r"^\s+CVNs\s*\(\d+\):", line)
        if m:
            section = "cvns"
            continue
        m = re.match(r"^\s+\$1A B4:\s+(.+?)\s*$", line)
        if m:
            cur["lid_b4"] = m.group(1)
            section = None
            continue
        m = re.match(r"^\s{4,}(\d+):\s+(.+?)\s*$", line)
        if m:
            value = m.group(2)
            if section == "calids":
                cur["cal_ids"].append(value)
            elif section == "cvns":
                cur["cvns"].append(value)
            continue

    if cur is not None:
        modules.append(cur)
    return modules


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=500000)
    ap.add_argument("--json", action="store_true",
                    help="emit parsed JSON instead of pass-through text")
    args = ap.parse_args()

    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    raw = drive(args.port, args.baud)

    if args.json:
        modules = parse(raw)
        json.dump({"modules": modules}, sys.stdout, indent=2)
        sys.stdout.write("\n")
    else:
        sys.stdout.write(raw)


if __name__ == "__main__":
    main()
