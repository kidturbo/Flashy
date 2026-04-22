#!/usr/bin/env python3
"""
capture_bus.py - CAN bus capture -> SavvyCAN-format CSV.

Drives the firmware's CAPTURE command: puts the Feather in accept-all
mode and streams every received frame to a CSV file compatible with the
extract_kernel*.py tools (and SavvyCAN itself).

Usage:
    python capture_bus.py                           # 30 s capture -> capture.csv
    python capture_bus.py --out myrun.csv
    python capture_bus.py --duration 60000          # 60 s
    python capture_bus.py --port COM38 --baud 500000
    python capture_bus.py --no-init                 # skip INIT (bus already up)

Ctrl+C during capture sends a stop byte and flushes the partial CSV.
"""

import argparse
import os
import signal
import sys
import time
from datetime import datetime

import serial

from detect_port import find_feather


def default_out_path():
    """Default to ~/Desktop/capture-YYYYMMDD-HHMMSS.csv if Desktop exists,
    else ./capture.csv in the current working directory."""
    desktop = os.path.join(os.path.expanduser('~'), 'Desktop')
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    if os.path.isdir(desktop):
        return os.path.join(desktop, f'capture-{stamp}.csv')
    return f'capture-{stamp}.csv'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('--port', help='Serial port (auto-detect if omitted)')
    ap.add_argument('--baud', type=int, default=500000,
                    help='CAN bus baud rate (default 500000)')
    ap.add_argument('--duration', type=int, default=30000,
                    help='Capture duration in ms (default 30000, max 600000)')
    ap.add_argument('--out', default=None,
                    help='Output CSV path (default: ~/Desktop/capture-<timestamp>.csv)')
    ap.add_argument('--no-init', action='store_true',
                    help='Skip INIT (firmware CAN already initialized)')
    args = ap.parse_args()
    if args.out is None:
        args.out = default_out_path()

    port = args.port or find_feather()
    if not port:
        print('ERROR: No Feather found. Specify --port COMx.', file=sys.stderr)
        sys.exit(1)

    print(f'Port:     {port}')
    print(f'Baud:     {args.baud}')
    print(f'Duration: {args.duration} ms')
    print(f'Output:   {args.out}')

    ser = serial.Serial(port, 115200, timeout=0.05)
    time.sleep(0.5)
    ser.reset_input_buffer()

    if not args.no_init:
        ser.write(f'INIT {args.baud}\n'.encode())
        ser.flush()
        deadline = time.time() + 2.0
        while time.time() < deadline:
            line = ser.readline().decode(errors='replace').rstrip()
            if not line:
                break
            print(f'  {line}')

    ser.write(f'CAPTURE {args.duration}\n'.encode())
    ser.flush()

    stop = {'flag': False, 'sent_q': False}

    def on_sigint(sig, frame):
        if not stop['sent_q']:
            print('\nCtrl+C - telling firmware to stop...', file=sys.stderr)
            try:
                ser.write(b'\n')
                ser.flush()
            except Exception:
                pass
            stop['sent_q'] = True
        else:
            print('\nForce exit.', file=sys.stderr)
            stop['flag'] = True

    signal.signal(signal.SIGINT, on_sigint)

    # Chunked-read state machine. readline() is byte-at-a-time and too
    # slow at ~1000 frames/s — causes USB-CDC backpressure that wedges
    # the firmware's Serial.println. Instead, drain whatever's available
    # each pass and split on newlines in memory.
    buf = bytearray()
    frame_count = 0
    started = False
    done = False
    t0 = time.time()

    with open(args.out, 'w', newline='', encoding='ascii', buffering=1 << 20) as f:
        deadline = time.time() + (args.duration / 1000.0) + 10.0
        while not done and not stop['flag'] and time.time() < deadline:
            chunk = ser.read(max(1, ser.in_waiting))
            if not chunk:
                continue
            buf.extend(chunk)

            while True:
                nl = buf.find(b'\n')
                if nl < 0:
                    break
                line = bytes(buf[:nl]).rstrip(b'\r').decode(errors='replace')
                del buf[:nl + 1]

                if not line:
                    continue

                if not started:
                    print(line)
                    if line.startswith('CAPTURE_START'):
                        started = True
                    elif line.startswith('ERR'):
                        print(f'ERROR from firmware: {line}', file=sys.stderr)
                        sys.exit(2)
                    continue

                if line.startswith('CAPTURE_DONE'):
                    print(line)
                    done = True
                    break

                if line.startswith('OK'):
                    continue

                f.write(line + '\n')
                if ',' in line and not line.startswith('Time Stamp'):
                    frame_count += 1
                    if frame_count % 2000 == 0:
                        elapsed = time.time() - t0
                        print(f'  {frame_count:6d} frames  ({frame_count/elapsed:.0f} frames/s)')

    elapsed = time.time() - t0
    print(f'\nWrote {frame_count} frames to {args.out} in {elapsed:.1f}s.')
    if not done:
        print('(stopped before CAPTURE_DONE — partial capture)', file=sys.stderr)


if __name__ == '__main__':
    main()
