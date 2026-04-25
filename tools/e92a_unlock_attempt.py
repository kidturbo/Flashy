#!/usr/bin/env python3
"""Drive Feather to AUTH on E92A LATE — uses firmware's built-in algo 146."""
import io, os, sys, time, serial
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8',
                              errors='replace', line_buffering=True)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from detect_port import find_feather_or_prompt


def drain(ser, settle=0.4):
    end = time.time() + settle
    while time.time() < end:
        c = ser.read(4096)
        if not c: break
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


def main():
    port = find_feather_or_prompt()
    if not port: return 2
    ser = serial.Serial(port, 115200, timeout=0.1)
    try:
        time.sleep(1.0); drain(ser)
        send(ser, 'X');             read_until(ser, 2.0); drain(ser)
        send(ser, 'INIT 500000');   read_until(ser, 6.0)
        send(ser, 'ALGO E92');      read_until(ser, 4.0)
        send(ser, 'E92ID');         read_until(ser, 8.0)
        send(ser, 'AUTH');          lines = read_until(ser, 10.0)

        unlocked = any('Security unlocked' in ln for ln in lines)
        print()
        print('=' * 60)
        if unlocked:
            print('  *** E92A UNLOCK CONFIRMED via algo 146 ***')
        else:
            print('  AUTH did not unlock — check NRC above for next step')
        print('=' * 60)
    finally:
        ser.close()


if __name__ == '__main__':
    sys.exit(main())
