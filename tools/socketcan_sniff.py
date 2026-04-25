#!/usr/bin/env python3
"""socketcan_sniff.py — connect to a socketcand daemon (Linux can-utils)
running on the bench Raspberry Pi and log raw CAN frames to a CSV that
matches the SavvyCAN column layout so the same analysis tools work.

socketcand protocol (plain-text over TCP):
    < open <bus> >          select CAN interface (e.g. can0)
    < rawmode >             switch to raw frame streaming
    < frame <ID> <ts> <D0> <D1> ... >   received frame
    < send <ID> <ext> <len> <D0> ... >  transmit frame (we rarely TX here)

Usage:
    python tools/socketcan_sniff.py --host 192.168.x.y --port 29536 \\
        --bus can0 --out captures/T3.csv --duration 120

Prints one line to the console every time a frame arrives with wallclock
timestamp so you can correlate with flashy_diag.py TX events.
"""
import socket
import argparse
import csv
import time
import sys
import re
import os


FRAME_RE = re.compile(r'<\s*frame\s+([0-9a-fA-F]+)\s+([0-9.]+)\s+([0-9a-fA-F]*?)\s*>')


def wallclock():
    t = time.time()
    ms = int((t - int(t)) * 1000)
    return time.strftime('%H:%M:%S', time.localtime(t)) + f'.{ms:03d}'


def savvycan_row(start_us, frame_id, ts_sec_str, data_hex):
    """Return a list of column values matching SavvyCAN CSV layout:
       Time Stamp, ID, Extended, Dir, Bus, LEN, D1..D8
    Time Stamp is in microseconds from an arbitrary epoch (SavvyCAN style)."""
    ts_us = int(float(ts_sec_str) * 1_000_000) if ts_sec_str else int(time.time() * 1_000_000)
    # socketcand packs payload as continuous hex (e.g. "80004000000000FF").
    # Also accept space-separated (future-proof).
    hex_s = data_hex.replace(' ', '').upper()
    bytes_list = [hex_s[i:i+2] for i in range(0, len(hex_s), 2)]
    length = len(bytes_list)
    ext = 'false' if len(frame_id) <= 3 else 'true'
    row = [str(ts_us), frame_id.upper().zfill(8), ext, 'Rx', '0', str(length)]
    for i in range(8):
        row.append(bytes_list[i] if i < length else '')
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', required=True)
    ap.add_argument('--port', type=int, default=29536)
    ap.add_argument('--bus', default='can0')
    ap.add_argument('--out', required=True, help='CSV output path')
    ap.add_argument('--duration', type=float, default=120.0,
                    help='seconds to capture (default 120)')
    ap.add_argument('--print', action='store_true',
                    help='also print every frame to console')
    args = ap.parse_args()

    print(f'[connecting to socketcand {args.host}:{args.port} bus={args.bus}]')
    s = socket.create_connection((args.host, args.port), timeout=5.0)
    s.settimeout(1.0)

    # socketcand prints a banner, then expects commands
    try:
        banner = s.recv(256).decode('ascii', errors='replace')
        print(f'[banner] {banner.strip()}')
    except socket.timeout:
        pass

    def send_cmd(cmd):
        s.sendall(f'< {cmd} >'.encode('ascii'))
        time.sleep(0.05)

    send_cmd(f'open {args.bus}')
    time.sleep(0.15)
    send_cmd('rawmode')
    time.sleep(0.15)

    # drain any immediate acks
    try:
        ack = s.recv(512).decode('ascii', errors='replace')
        print(f'[ack] {ack.strip()}')
    except socket.timeout:
        pass

    # Open CSV
    os.makedirs(os.path.dirname(args.out) or '.', exist_ok=True)
    f = open(args.out, 'w', newline='')
    wr = csv.writer(f)
    wr.writerow(['Time Stamp', 'ID', 'Extended', 'Dir', 'Bus', 'LEN',
                 'D1', 'D2', 'D3', 'D4', 'D5', 'D6', 'D7', 'D8'])

    start_us = int(time.time() * 1_000_000)
    end_t = time.time() + args.duration
    buf = ''
    frames = 0
    print(f'[capture start, duration={args.duration}s, writing {args.out}]')
    print(f'[wallclock {wallclock()} — send CANSEND commands via flashy_diag.py now]')

    try:
        while time.time() < end_t:
            try:
                chunk = s.recv(4096).decode('ascii', errors='replace')
                if not chunk:
                    break
                buf += chunk
            except socket.timeout:
                continue

            while True:
                m = FRAME_RE.search(buf)
                if not m:
                    break
                frame_id = m.group(1)
                ts_str   = m.group(2)
                data_hex = m.group(3)
                row = savvycan_row(start_us, frame_id, ts_str, data_hex)
                wr.writerow(row)
                f.flush()
                frames += 1
                if args.print:
                    print(f'  [{wallclock()}] 0x{frame_id.upper()} LEN={row[5]}  {data_hex}')
                buf = buf[m.end():]
    finally:
        s.close()
        f.close()
        print(f'[capture done, {frames} frames in {args.duration:.1f}s -> {args.out}]')


if __name__ == '__main__':
    main()
