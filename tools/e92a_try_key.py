#!/usr/bin/env python3
"""One-shot $27 02 <key> attempt on a primed E92A. Reports response."""
import argparse, io, os, sys, time, serial
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8',
                              errors='replace', line_buffering=True)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from detect_port import find_feather_or_prompt


def drain(ser, settle=0.3):
    end = time.time() + settle
    out = bytearray()
    while time.time() < end:
        c = ser.read(4096)
        if c: out += c; end = time.time() + settle
    return out.decode('ascii', errors='replace')


def send(ser, line):
    print(f'  > {line}')
    ser.write((line + '\r\n').encode('ascii'))


def read_until(ser, timeout=8.0):
    lines = []; buf = b''; end = time.time() + timeout
    while time.time() < end:
        c = ser.read(512)
        if not c: continue
        buf += c
        while b'\n' in buf:
            raw, buf = buf.split(b'\n', 1)
            ln = raw.decode('ascii', errors='replace').rstrip('\r').rstrip()
            if ln.startswith('> '): ln = ln[2:]
            if not ln: continue
            print(f'  < {ln}')
            lines.append(ln)
            if ln == 'OK' or ln.startswith('ERR:'): return lines
    return lines


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--port')
    p.add_argument('--level', type=lambda x: int(x, 0), default=0x02,
                   help='even level for sendKey (default 0x02)')
    p.add_argument('--key', required=True, help='5-byte key hex, e.g. 6E9B8B6AE7')
    args = p.parse_args()

    key = bytes.fromhex(args.key)
    port = args.port or find_feather_or_prompt()
    if not port: return 2
    ser = serial.Serial(port, 115200, timeout=0.1)
    try:
        time.sleep(1.0); drain(ser)
        send(ser, 'X');                    read_until(ser, 2.0); drain(ser)
        send(ser, 'INIT 500000');          read_until(ser, 6.0)
        send(ser, 'ALGO E92');             read_until(ser, 4.0)

        # Re-fetch the seed FIRST so we can verify it matches what was used
        # for the precomputed key. Send raw $27 01 directly.
        send(ser, 'RAW 27 01')
        seed_lines = read_until(ser, 6.0)
        seed_hex = None
        for ln in seed_lines:
            if 'RECV:' in ln.upper() or 'RX:' in ln.upper() or 'RESP:' in ln.upper():
                # try to find 67 01 followed by hex
                pass
            # Flashy's RAW print format includes the response bytes inline
            up = ln.upper().replace(' ', '').replace(':', '')
            if '6701' in up:
                idx = up.index('6701') + 4
                seed_hex = up[idx:idx+10]  # 5 bytes = 10 hex chars
                break
        print()
        print(f'  current $27 01 seed = {seed_hex or "(unable to parse)"}')
        print(f'  attempting key      = {args.key.upper()} (level {args.level:02X})')
        print()

        # Submit the key
        cmd = f'RAW 27 {args.level:02X} ' + ' '.join(f'{b:02X}' for b in key)
        send(ser, cmd)
        result_lines = read_until(ser, 8.0)

        verdict = '???'
        for ln in result_lines:
            up = ln.upper().replace(' ', '').replace(':', '')
            if '67' + f'{args.level:02X}' in up:
                verdict = 'UNLOCKED — key accepted'
                break
            if '7F27' in up:
                pos = up.index('7F27') + 4
                if pos + 2 <= len(up):
                    nrc = up[pos:pos+2]
                    verdict = f'NRC 0x{nrc} — '
                    table = {'35': 'invalidKey (wrong algo or wrong key) — 1 MEC tick spent',
                             '36': 'exceededAttempts (LOCKOUT — STOP)',
                             '37': 'requiredTimeDelay (short-term, wait 10s)',
                             '12': 'subFunctionNotSupported (length/format issue)',
                             '24': 'requestSequenceError (re-request seed first)',
                             '13': 'incorrectMessageLengthOrInvalidFormat'}
                    verdict += table.get(nrc, 'unknown — see ISO 14229 NRC table')
                break

        print()
        print('=' * 60)
        print(f'VERDICT: {verdict}')
        print('=' * 60)
    finally:
        ser.close()


if __name__ == '__main__':
    sys.exit(main())
