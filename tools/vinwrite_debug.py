#!/usr/bin/env python3
"""
vinwrite_debug.py — Methodically debug VINWRITE on the Feather.

Runs the VINWRITE command through escalating prep stages so the log shows
exactly which gate (session, auth) the ECU needs. Stops at the first stage
that succeeds.

Stages:
  A. bare VINWRITE                            (default session, no auth)
  B. DIAG 03 + VINWRITE                       (extended session)
  C. DIAG 03 + AUTH + VINWRITE                (extended session + unlock)
  D. DIAG 02 + AUTH + VINWRITE                (programming session + unlock)

Usage:
    python tools/vinwrite_debug.py 1G1XXXXXXXXXXXXXX
    python tools/vinwrite_debug.py 1G1XXXXXXXXXXXXXX --port COM38 --module e92

Requires: pip install pyserial
"""

import argparse
import datetime as dt
import io
import os
import sys
import time

import serial

# Firmware emits em-dashes etc. — reopen stdout as UTF-8 (Windows default
# cp1252 can't encode them and would crash mid-run).
try:
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8',
                                  errors='replace', line_buffering=True)
except Exception:
    pass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from detect_port import find_feather_or_prompt


MODULES = {
    # key -> (ALGO arg for firmware, human label)
    'e38':  ('E38',  'E38 ECM'),
    'e67':  ('E67',  'E67 ECM'),
    'e92':  ('E92',  'E92 ECM'),
    't87':  ('T87',  'T87 TCM'),
    't87a': ('T87',  'T87A TCM'),
    't93':  ('T93',  'T93 TCM'),
    't42':  ('T42',  'T42 TCM'),
}


class Session:
    def __init__(self, port, baud=115200, logf=None):
        self.ser = serial.Serial(port, baud, timeout=0.1)
        self.logf = logf

    def close(self):
        try:
            self.ser.close()
        finally:
            if self.logf:
                self.logf.close()

    def _log(self, s):
        line = s.rstrip()
        print(line)
        if self.logf:
            self.logf.write(line + '\n')
            self.logf.flush()

    def drain(self, settle=0.5):
        """Consume anything currently queued; return collected text."""
        end = time.time() + settle
        buf = []
        while time.time() < end:
            chunk = self.ser.read(4096)
            if chunk:
                buf.append(chunk.decode('ascii', errors='replace'))
                end = time.time() + settle
        text = ''.join(buf)
        for line in text.splitlines():
            if line.strip():
                self._log(f'  < {line}')
        return text

    def send(self, cmd):
        self._log(f'  > {cmd}')
        self.ser.write((cmd + '\r\n').encode('ascii'))

    def read_until(self, terminators=('OK', 'ERR:'), timeout=10.0,
                   also_stop_on=None):
        """Read lines until one matches a terminator prefix. Returns the list
        of stripped lines read (including the terminator line)."""
        lines = []
        buf = b''
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = self.ser.read(512)
            if not chunk:
                continue
            buf += chunk
            while b'\n' in buf:
                raw, buf = buf.split(b'\n', 1)
                line = raw.decode('ascii', errors='replace').rstrip('\r').rstrip()
                # strip the "> " prompt the firmware prints after each command
                if line.startswith('> '):
                    line = line[2:]
                if not line:
                    continue
                self._log(f'  < {line}')
                lines.append(line)
                if line == 'OK' or line.startswith('ERR:'):
                    return lines
                if also_stop_on and any(line.startswith(p) for p in also_stop_on):
                    return lines
        lines.append('<read timeout>')
        self._log('  < <read timeout>')
        return lines


def result_of(lines):
    """Summarise a command's reply: 'ok' | 'err:<msg>' | 'nrc:<hex>' | 'timeout'."""
    for line in reversed(lines):
        if line == 'OK':
            return 'ok'
        if line.startswith('ERR: NRC 0x'):
            return 'nrc:' + line.split('0x', 1)[1].strip()
        if line.startswith('ERR:'):
            return 'err:' + line[4:].strip()
        if line == '<read timeout>':
            return 'timeout'
    return 'unknown'


def run_vinwrite_stage(s, label, prep_steps, new_vin):
    print()
    print(f'==== {label} ====')
    for cmd in prep_steps:
        s.send(cmd)
        s.read_until(timeout=6.0)
    s.send(f'VINWRITE {new_vin}')
    # VINWRITE can emit multi-line output (Writing VIN:, hints) before OK/ERR.
    lines = s.read_until(timeout=15.0)
    verdict = result_of(lines)
    print(f'---- {label} verdict: {verdict} ----')
    return verdict, lines


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('vin', help='17-char VIN to write')
    p.add_argument('--port', help='COM port (auto-detect if omitted)')
    p.add_argument('--module', default='e92',
                   choices=sorted(MODULES.keys()),
                   help='Target module (default: e92)')
    p.add_argument('--baud', type=int, default=500000,
                   help='CAN bus baud (default: 500000)')
    p.add_argument('--log', help='Write transcript to this file '
                                 '(default: vinwrite_debug_<ts>.log)')
    args = p.parse_args()

    if len(args.vin) != 17:
        print(f'ERR: VIN must be 17 chars, got {len(args.vin)}')
        return 2

    port = args.port or find_feather_or_prompt()
    if not port:
        print('ERR: no serial port')
        return 2

    ts = dt.datetime.now().strftime('%Y%m%d_%H%M%S')
    log_path = args.log or f'vinwrite_debug_{ts}.log'
    logf = open(log_path, 'w', encoding='utf-8')
    print(f'Log: {log_path}')
    print(f'Port: {port}  Module: {args.module}  Target VIN: {args.vin}')

    s = Session(port, logf=logf)
    try:
        # Let the Feather's auto-CAN-detect banner finish, then force out of
        # the auto-launched menu.
        time.sleep(2.0)
        s.drain(0.3)
        s.send('X')            # exit menu from any level (harmless if off)
        s.read_until(timeout=1.5,
                     also_stop_on=('Menu exited', 'RAW command',
                                   'Unknown command'))
        s.drain(0.3)

        # Baseline connection: baud + module IDs + algo.
        algo_arg, label = MODULES[args.module]
        print(f'\n== Setup: {label} ==')
        s.send(f'INIT {args.baud}')
        s.read_until(timeout=6.0)
        s.send(f'ALGO {algo_arg}')
        s.read_until(timeout=4.0)
        s.send('VIN')
        vin_lines = s.read_until(timeout=6.0)
        current = None
        for ln in vin_lines:
            if ln.startswith('VIN:'):
                current = ln[4:].strip()
                break
        print(f'  current VIN: {current}')
        if current == args.vin:
            print('  target matches current — VINWRITE will short-circuit.')

        # Escalating stages. Stop at first OK.
        stages = [
            ('Stage A — bare VINWRITE', ['DIAG 01']),
            ('Stage B — DIAG 03 + VINWRITE', ['DIAG 03']),
            ('Stage C — DIAG 03 + AUTH + VINWRITE', ['DIAG 03', 'AUTH']),
            ('Stage D — DIAG 02 + AUTH + VINWRITE', ['DIAG 02', 'AUTH']),
        ]

        results = []
        for label, prep in stages:
            verdict, _ = run_vinwrite_stage(s, label, prep, args.vin)
            results.append((label, verdict))
            if verdict == 'ok':
                break

        # Cleanup: drop back to default session.
        s.send('DIAG 01')
        s.read_until(timeout=4.0)

        print('\n==== Summary ====')
        for label, verdict in results:
            print(f'  {verdict:>14}  {label}')
        final = results[-1][1] if results else 'unknown'
        if final == 'ok':
            print('\nVIN write SUCCEEDED. Minimal prep = the last stage run.')
        else:
            print('\nAll stages failed. See NRCs above; common meanings:')
            print('  NRC 0x7F — service not supported in active session')
            print('  NRC 0x11 — service not supported')
            print('  NRC 0x12 — sub-function not supported')
            print('  NRC 0x13 — incorrect message length/format')
            print('  NRC 0x22 — conditions not correct (e.g. RPM/ignition)')
            print('  NRC 0x33 — security access denied (needs AUTH)')
            print('  NRC 0x7E — sub-function not supported in active session')
        return 0 if final == 'ok' else 1
    finally:
        s.close()


if __name__ == '__main__':
    sys.exit(main())
