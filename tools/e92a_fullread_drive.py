#!/usr/bin/env python3
"""Drive E92FULLREAD on LATE unit (algo 146 path). ~7 min runtime."""
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


def stream_until_done(ser, end_markers, hard_errors=('aborting', 'failed to start'),
                      timeout=900.0, quiet_prefixes=('DATA:',), idle_max=120.0):
    """Stream lines until any end_marker substring appears, or hard error,
    or idle gap > idle_max with no traffic. Suppresses noisy DATA: prefix."""
    lines = []; buf = b''
    end = time.time() + timeout
    last_rx = time.time()
    quiet_count = 0
    while time.time() < end:
        c = ser.read(2048)
        if c:
            last_rx = time.time()
            buf += c
            while b'\n' in buf:
                raw, buf = buf.split(b'\n', 1)
                ln = raw.decode('ascii', errors='replace').rstrip('\r').rstrip()
                if ln.startswith('> '): ln = ln[2:]
                if not ln: continue
                lines.append(ln)
                if any(ln.startswith(p) for p in quiet_prefixes):
                    quiet_count += 1
                    if quiet_count % 256 == 0:
                        print(f'    ... DATA blocks streamed: {quiet_count}')
                    continue
                print(f'  < {ln}')
                if any(m in ln for m in end_markers):
                    print(f'    (DATA: lines suppressed: {quiet_count})')
                    return lines
                if any(e in ln.lower() for e in hard_errors):
                    print(f'    HARD ERROR: {ln}')
                    print(f'    (DATA: lines suppressed: {quiet_count})')
                    return lines
        else:
            idle = time.time() - last_rx
            if idle > idle_max:
                print(f'    IDLE {idle:.0f}s > {idle_max:.0f}s, giving up')
                return lines
            if idle > 30 and int(idle) % 30 == 0:
                # heartbeat once a minute-ish
                pass
    print(f'    TIMEOUT after {timeout}s; suppressed: {quiet_count}')
    return lines


def main():
    port = find_feather_or_prompt()
    if not port: return 2
    ser = serial.Serial(port, 115200, timeout=0.5)
    try:
        time.sleep(1.0); drain(ser)
        send(ser, 'X'); time.sleep(0.5); drain(ser)
        send(ser, 'E92FULLREAD')
        # End markers: full read finishes with reset sequence then a final
        # "normal bus traffic should resume" line. Also catch SD save line
        # in case reset path differs.
        lines = stream_until_done(
            ser,
            end_markers=('normal bus traffic should resume',
                         'KERNELREAD: done',
                         'E92FULLREAD aborted'),
            hard_errors=('failed to start', 'kernel failed', 'No module selected'),
            timeout=900.0,
            idle_max=180.0)

        success = any('SD: file size' in ln or 'KERNELREAD: done' in ln for ln in lines)
        size_line = next((ln for ln in lines if 'SD: file size' in ln), None)
        chk_line  = next((ln for ln in lines if 'checksum' in ln.lower()), None)
        path_line = next((ln for ln in lines if ln.startswith('SD: ')), None)

        print()
        print('=' * 60)
        if success:
            print('  *** E92A FULL READ COMPLETE ***')
            if path_line: print(f'  {path_line}')
            if size_line: print(f'  {size_line}')
            if chk_line:  print(f'  {chk_line}')
        else:
            print('  Read did not complete — check log above')
        print('=' * 60)
    finally:
        ser.close()


if __name__ == '__main__':
    sys.exit(main())
