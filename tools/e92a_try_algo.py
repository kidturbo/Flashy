#!/usr/bin/env python3
"""Fetch fresh seed, compute key for given algo, submit. One MEC tick if wrong."""
import argparse, io, os, re, sys, time, serial
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8',
                              errors='replace', line_buffering=True)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gm5byte'))
from detect_port import find_feather_or_prompt
import keylib


def drain(ser, settle=0.3):
    end = time.time() + settle
    while time.time() < end:
        if not ser.read(4096): break
        end = time.time() + settle


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


def parse_seed_from_rsp(lines):
    """Flashy's RAW response format: 'RSP: 67 1 | 87 85 EE C1 06'."""
    for ln in lines:
        m = re.search(r'RSP:\s*67\s+1\s*\|\s*([0-9A-Fa-f\s]+)', ln)
        if m:
            return bytes(int(b, 16) for b in m.group(1).split() if b.strip())
    return None


def parse_verdict(lines, level):
    """Returns ('ok'|'nrc:XX'|'err:msg'|'unknown', detail)."""
    for ln in lines:
        if f'RSP: 67 {level:X} ' in ln or f'RSP: 67 {level:02X} ' in ln:
            return 'ok', 'key accepted, ECU unlocked'
        m = re.search(r'NRC\s*0x([0-9A-Fa-f]{2})', ln)
        if m:
            return f'nrc:{m.group(1).upper()}', ln
    for ln in lines:
        if ln.startswith('ERR:'):
            return 'err', ln
    return 'unknown', '(no parsable result)'


NRC_TABLE = {
    '12': 'subFunctionNotSupported (length/format issue)',
    '13': 'incorrectMessageLengthOrInvalidFormat',
    '24': 'requestSequenceError (re-request seed first)',
    '35': 'invalidKey — wrong algo or wrong key (1 MEC tick spent)',
    '36': 'exceededAttempts — LOCKOUT (stop, do not retry)',
    '37': 'requiredTimeDelay — short-term, wait ~10s',
}


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--port')
    p.add_argument('--algo', type=int, required=True, help='5-byte algo index (135, 137, 139, 141, ...)')
    p.add_argument('--level', type=lambda x: int(x, 0), default=0x01,
                   help='request-seed level (default 0x01); send-key = level+1')
    args = p.parse_args()

    port = args.port or find_feather_or_prompt()
    if not port: return 2
    ser = serial.Serial(port, 115200, timeout=0.1)
    try:
        time.sleep(1.0); drain(ser)
        send(ser, 'X');                    read_until(ser, 2.0); drain(ser)
        send(ser, 'INIT 500000');          read_until(ser, 6.0)
        send(ser, 'ALGO E92');             read_until(ser, 4.0)

        # Fresh seed from this boot.
        send(ser, f'RAW 27 {args.level:02X}')
        seed_lines = read_until(ser, 6.0)
        seed = parse_seed_from_rsp(seed_lines)
        if not seed:
            print('\nERR: could not parse seed from $27 response')
            return 1
        print(f'\n  fresh seed (level {args.level:02X}): {seed.hex().upper()}  ({len(seed)} bytes)')

        # Compute key.
        key, n_iter, mid = keylib.derive_key_from_algo(args.algo, seed)
        print(f'  algo {args.algo}: key = {key.hex().upper()}  (iter={n_iter})')

        # Submit.
        send_level = args.level + 1
        cmd = f'RAW 27 {send_level:02X} ' + ' '.join(f'{b:02X}' for b in key)
        send(ser, cmd)
        result_lines = read_until(ser, 8.0)

        verdict, detail = parse_verdict(result_lines, send_level)
        print()
        print('=' * 60)
        if verdict == 'ok':
            print(f'  *** UNLOCKED *** algo {args.algo} is the E92A unlock!')
        elif verdict.startswith('nrc:'):
            nrc = verdict.split(':', 1)[1]
            meaning = NRC_TABLE.get(nrc, '(see ISO 14229)')
            print(f'  NRC 0x{nrc} — {meaning}')
            print(f'  → algo {args.algo} is NOT the E92A algo')
        else:
            print(f'  {verdict.upper()} — {detail}')
        print('=' * 60)
    finally:
        ser.close()


if __name__ == '__main__':
    sys.exit(main())
