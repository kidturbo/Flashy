#!/usr/bin/env python3
"""
e92a_seedkey_test.py — Capture fresh E92A seed, compute candidate keys.

Drives the Feather to:
  1. Exit menu, INIT 500000, ALGO E92, E92ID
  2. Run E92SAPROBE; parse out the $27 01 5-byte seed
  3. Locally compute candidate keys for the top GM 5-byte algo indices
     (135 leading hypothesis, 137/139/141 siblings)
  4. STOP before any $27 02 — caller decides whether to send the key.

No bus-side risk: only request-seed traffic (level 01/03 sweep). MEC
counter is untouched.

Usage:
    python tools/e92a_seedkey_test.py
    python tools/e92a_seedkey_test.py --port COM13 --algos 135,137,139,141
"""

import argparse
import io
import os
import re
import sys
import time

import serial

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8',
                              errors='replace', line_buffering=True)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gm5byte'))
from detect_port import find_feather_or_prompt
import keylib


def drain(ser, settle=0.4):
    end = time.time() + settle
    text = bytearray()
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            text += chunk
            end = time.time() + settle
    return text.decode('ascii', errors='replace')


def send(ser, line):
    print(f'  > {line}')
    ser.write((line + '\r\n').encode('ascii'))


def read_until(ser, terminators=('OK', 'ERR:'), timeout=10.0):
    """Read lines until one matches a terminator prefix. Returns lines list."""
    lines = []
    buf = b''
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(512)
        if not chunk:
            continue
        buf += chunk
        while b'\n' in buf:
            raw, buf = buf.split(b'\n', 1)
            line = raw.decode('ascii', errors='replace').rstrip('\r').rstrip()
            if line.startswith('> '):
                line = line[2:]
            if not line:
                continue
            print(f'  < {line}')
            lines.append(line)
            if line == 'OK' or line.startswith('ERR:'):
                return lines
    print('  < <read timeout>')
    return lines


def parse_saprobe_seeds(lines):
    """Find SEED(NB)=HEX entries by level. Returns dict {level: bytes}."""
    seeds = {}
    pat = re.compile(r'\$27\s+([0-9A-F]{2}):\s+SEED\((\d+)B\)=([0-9A-F]+)',
                     re.IGNORECASE)
    for ln in lines:
        m = pat.search(ln)
        if not m:
            continue
        level = int(m.group(1), 16)
        nbytes = int(m.group(2))
        hex_ = m.group(3)
        if len(hex_) // 2 == nbytes:
            seeds[level] = bytes.fromhex(hex_)
    return seeds


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--port', help='COM port (auto-detect if omitted)')
    p.add_argument('--algos', default='135,137,139,141',
                   help='Comma-separated algo indices to compute (default: 135,137,139,141)')
    p.add_argument('--baud', type=int, default=500000,
                   help='CAN bus baud (default 500000)')
    args = p.parse_args()

    algos = [int(s) for s in args.algos.split(',')]

    port = args.port or find_feather_or_prompt()
    if not port:
        print('ERR: no serial port'); return 2

    print(f'Port: {port}')
    print(f'Algos to compute: {algos}')
    ser = serial.Serial(port, 115200, timeout=0.1)
    try:
        time.sleep(1.5)
        drain(ser)

        send(ser, 'X')
        read_until(ser, timeout=2.0)
        drain(ser)

        send(ser, f'INIT {args.baud}')
        read_until(ser, timeout=6.0)

        send(ser, 'ALGO E92')
        read_until(ser, timeout=4.0)

        send(ser, 'E92ID')
        read_until(ser, timeout=8.0)

        send(ser, 'E92SAPROBE')
        # E92SAPROBE prints ~12 lines + "OK" — give it generous timeout
        # since it does 10 round-trips with 50ms gaps + some ECU latency.
        lines = read_until(ser, timeout=20.0)

        seeds = parse_saprobe_seeds(lines)
        print()
        print('=' * 60)
        print('Captured seeds:')
        if not seeds:
            print('  NONE — E92SAPROBE returned no parseable seeds.')
            print('  Check the firmware is current and the LATE unit is talking.')
            return 1
        for lvl, s in sorted(seeds.items()):
            print(f'  $27 {lvl:02X}: {s.hex().upper()}  ({len(s)} bytes)')

        # Pick the seed for level 01 by default (canonical entry point).
        target_level = 0x01 if 0x01 in seeds else min(seeds.keys())
        target_seed  = seeds[target_level]
        print()
        print(f'Computing candidate keys for $27 {target_level:02X} seed = {target_seed.hex().upper()}')
        print('=' * 60)

        for algo in algos:
            try:
                key, n_iter, mid = keylib.derive_key_from_algo(algo, target_seed)
                print(f'  algo {algo:>3d}: KEY={key.hex().upper()}  '
                      f'(iter={n_iter}, mid={mid.hex().upper()[:16]}...)')
            except Exception as e:
                print(f'  algo {algo:>3d}: ERROR {e}')

        print()
        print('=' * 60)
        print('NEXT STEP (manual, MEC-tick risk = 1 if wrong):')
        print(f'  RAW 27 {target_level + 1:02X} <KEY-BYTES>')
        print('  e.g. for algo 135:')
        try:
            key135, _, _ = keylib.derive_key_from_algo(135, target_seed)
            print(f'    RAW 27 {target_level + 1:02X} {key135.hex().upper()}')
        except Exception:
            pass
        print()
        print('  ECU response interpretation:')
        print('    67 02              -> UNLOCKED. Algo confirmed.')
        print('    7F 27 35           -> NRC 0x35 invalidKey -> wrong algo.')
        print('    7F 27 36           -> NRC 0x36 exceededAttempts -> lockout, STOP.')
        print('    7F 27 37           -> NRC 0x37 requiredTimeDelay -> wait + retry.')
        print('    7F 27 12           -> NRC 0x12 still -> length mismatch (debug).')
        print()
        print('  WARNING: the seed re-randomizes per request on E92A. If you')
        print('  re-run E92SAPROBE between this script and the RAW command, the')
        print('  seed will change and the precomputed key will be stale.')

    finally:
        ser.close()


if __name__ == '__main__':
    sys.exit(main())
