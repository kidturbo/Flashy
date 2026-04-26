#!/usr/bin/env python3
"""set_rtc_local.py — Set Flashy's PCF8523 RTC from the PC's current local time.

Reads `datetime.datetime.now()` on the host, formats it as
`SETCLOCK YYMMDD HHMMSS`, and sends it to the Feather over serial.
Reads back the firmware's confirmation line.

Auto-detects the Feather by USB VID 0x239A if --port is not given.

Usage:
    python tools/set_rtc_local.py
    python tools/set_rtc_local.py --port COM13
    python tools/set_rtc_local.py --utc          # use UTC instead of local

Requires: pip install pyserial
"""
from __future__ import annotations

import argparse
import datetime as dt
import sys
import time

import serial
import serial.tools.list_ports


ADAFRUIT_VID = 0x239A


def auto_detect_port() -> str:
    """Find the single Adafruit Feather on the system, or raise."""
    candidates = [p for p in serial.tools.list_ports.comports() if p.vid == ADAFRUIT_VID]
    if not candidates:
        raise SystemExit(
            "No Adafruit Feather (VID 0x239A) found. Plug it in or pass --port explicitly."
        )
    if len(candidates) > 1:
        names = ", ".join(p.device for p in candidates)
        raise SystemExit(
            f"Multiple Adafruit devices found ({names}). Pick one with --port."
        )
    return candidates[0].device


def drive(port: str, when: dt.datetime) -> int:
    yymmdd = when.strftime("%y%m%d")
    hhmmss = when.strftime("%H%M%S")
    cmd = f"SETCLOCK {yymmdd} {hhmmss}"

    print(f"[opening {port} @ 115200]")
    s = serial.Serial(port, 115200, timeout=0.2)
    try:
        # Drain any boot banner / stale output
        time.sleep(0.5)
        s.reset_input_buffer()

        # Exit the interactive menu if it's active. The Flashy menu
        # auto-launches at boot and treats every input as a numbered
        # choice; SETCLOCK is not a number and gets rejected as
        # "Invalid choice." Sending X exits the menu cleanly. If the
        # menu is already off, X is processed by the dispatch as an
        # unknown command — harmless.
        s.write(b"X\n")
        s.flush()
        time.sleep(0.3)
        s.reset_input_buffer()

        print(f"[host time: {when.isoformat(sep=' ', timespec='seconds')}]")
        print(f"TX: {cmd}")
        s.write((cmd + "\n").encode())
        s.flush()

        # Collect 1.5 s of response — SETCLOCK is fast but we want all output.
        buf = b""
        t_end = time.monotonic() + 1.5
        while time.monotonic() < t_end:
            chunk = s.read(4096)
            if chunk:
                buf += chunk
        out = buf.decode("utf-8", errors="replace")
        sys.stdout.write(out)
        sys.stdout.flush()

        return 0 if "\nOK" in out or out.strip().endswith("OK") else 1
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", default=None,
                    help="serial port (auto-detect Adafruit Feather if omitted)")
    ap.add_argument("--utc", action="store_true",
                    help="use UTC instead of local time")
    args = ap.parse_args()

    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    port = args.port or auto_detect_port()
    when = dt.datetime.utcnow() if args.utc else dt.datetime.now()
    sys.exit(drive(port, when))


if __name__ == "__main__":
    main()
