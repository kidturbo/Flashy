#!/usr/bin/env python3
"""flashy_diag.py — send a sequence of Flashy serial commands and capture
output. Designed for Claude / bench operator to drive TCM recovery and
quick probes without the human needing to paste each line manually.

Usage:
    python tools/flashy_diag.py [--port COM13] [--baud 115200]
                                [--wait-after N] CMD1 [CMD2 ...]

Each CMD runs with a ~0.6 s drain window before the next is sent, giving
Flashy time to emit its banner. Per-command output is framed with
'=== CMD ===' headers so log parsing is easy.
"""
import serial
import sys
import time
import argparse


def drain(s, dur=0.3):
    end = time.time() + dur
    buf = b''
    while time.time() < end:
        if s.in_waiting:
            buf += s.read(s.in_waiting)
        else:
            time.sleep(0.02)
    return buf


def send_line(s, line):
    s.write((line + '\r').encode('ascii'))
    s.flush()


def wallclock():
    """HH:MM:SS.mmm — for cross-referencing CAN CSV captures."""
    t = time.time()
    ms = int((t - int(t)) * 1000)
    return time.strftime('%H:%M:%S', time.localtime(t)) + f'.{ms:03d}'


def run_cmd(s, cmd, wait_s):
    """Send one command and drain output for wait_s seconds.
    Wallclock timestamp before TX and after last-byte RX, so the CSV
    capture can be correlated to tester-side actions without restarting
    the trace. Early-exits when shell prompt returns."""
    ts_tx = wallclock()
    print(f'=== [{ts_tx}] TX: {cmd} ===')
    send_line(s, cmd)
    end = time.time() + wait_s
    buf = b''
    quiet_since = None
    while time.time() < end:
        chunk = s.read(256)
        if chunk:
            buf += chunk
            quiet_since = time.time()
        else:
            # If we've seen a '>' prompt AND no data for 400 ms, call it done
            if buf.rstrip().endswith(b'>') and quiet_since and (time.time() - quiet_since) > 0.4:
                break
            time.sleep(0.05)
    ts_rx = wallclock()
    text = buf.decode('ascii', errors='replace')
    print(text, end='' if text.endswith('\n') else '\n')
    print(f'--- [{ts_rx}] done ---')
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', required=True,
                    help='serial port, e.g. COM38 or /dev/ttyACM0')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--wait-after', type=float, default=0.6,
                    help='seconds to drain output after each command')
    ap.add_argument('cmds', nargs='+', help='commands to send (in order)')
    args = ap.parse_args()

    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

    print(f'[opening {args.port} @ {args.baud}]')
    try:
        s = serial.Serial(args.port, args.baud, timeout=0.2)
    except serial.SerialException as e:
        print(f'[FAIL opening {args.port}: {e}]')
        print('[hint: close any serial terminal holding the port]')
        sys.exit(1)

    time.sleep(0.3)
    stale = drain(s, 0.3)
    if stale:
        print('[stale output, ignored]')
        print(stale.decode('ascii', errors='replace'))

    # back out of any menu level quietly
    for _ in range(3):
        send_line(s, 'B')
        time.sleep(0.1)
    drain(s, 0.3)

    for cmd in args.cmds:
        run_cmd(s, cmd, args.wait_after)

    s.close()
    print('[diag done]')


if __name__ == '__main__':
    main()
