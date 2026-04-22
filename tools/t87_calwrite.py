#!/usr/bin/env python3
"""
t87_calwrite.py — T87 TCM calibration write via J2534 Pass-Thru Feather

Loads a 4MB .bin file, extracts the cal region (0x080000-0x17FFFF, 1MB),
sends it to the Feather which writes it to the TCM using the
calflash kernel protocol ($36 EE erase, $36 00 write blocks, $36 FF finalize).

After writing, captures the readback and verifies it matches.

Usage:
    python t87_calwrite.py COM38 firmware.bin
    python t87_calwrite.py --help

Requires: pip install pyserial
"""

import argparse
import io
import os
import re
import serial
import sys
import time

from detect_port import find_feather_or_prompt

# Force UTF-8 output on Windows
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace', line_buffering=True)

T87_CAL_START  = 0x080000
T87_CAL_SIZE   = 0x100000   # 1 MB (0x080000-0x17FFFF, calflash kernel range)
T87_CAL_BLOCKS = 512        # 0x100000 / 0x800
BLOCK_SIZE     = 0x800      # 2048 bytes


def send_cmd(ser, cmd):
    print(f"  > {cmd}")
    ser.write((cmd + '\r\n').encode('ascii'))
    time.sleep(0.1)


def read_lines_until(ser, pattern, timeout=30, stop_patterns=None):
    """Read lines until pattern matches. Returns (match, all_lines)."""
    lines = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode('ascii', errors='replace').strip()
        except Exception:
            continue
        if not line:
            continue
        print(f"  < {line}")
        lines.append(line)
        if re.search(pattern, line):
            return line, lines
        if stop_patterns:
            for sp in stop_patterns:
                if re.search(sp, line):
                    return None, lines
    return None, lines


def read_until_ok(ser, timeout=30):
    lines = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            line = ser.readline().decode('ascii', errors='replace').strip()
        except Exception:
            continue
        if not line:
            continue
        print(f"  < {line}")
        lines.append(line)
        if line == 'OK' or line.startswith('ERR:'):
            break
    return lines


def main():
    parser = argparse.ArgumentParser(description='T87 TCM calibration write')
    parser.add_argument('port', nargs='?', default=None,
                        help='Serial port (auto-detected if omitted)')
    parser.add_argument('binfile', help='4MB .bin file to write')
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--timeout', type=int, default=600,
                        help='Total timeout seconds (default 600)')
    parser.add_argument('--skip-verify', action='store_true',
                        help='Skip readback verification')
    args = parser.parse_args()

    # Load .bin file
    if not os.path.exists(args.binfile):
        print(f"ERROR: File not found: {args.binfile}")
        sys.exit(1)

    with open(args.binfile, 'rb') as f:
        full_bin = f.read()

    print(f"Loaded: {args.binfile} ({len(full_bin)} bytes, {len(full_bin)/1024/1024:.1f} MB)")

    if len(full_bin) < T87_CAL_START + T87_CAL_SIZE:
        print(f"ERROR: File too small. Need at least {T87_CAL_START + T87_CAL_SIZE} bytes "
              f"(got {len(full_bin)})")
        sys.exit(1)

    cal_data = full_bin[T87_CAL_START:T87_CAL_START + T87_CAL_SIZE]
    print(f"Cal region: 0x{T87_CAL_START:06X}-0x{T87_CAL_START + T87_CAL_SIZE - 1:06X} "
          f"({len(cal_data)} bytes, {T87_CAL_BLOCKS} blocks)")

    # Connect
    port = args.port or find_feather_or_prompt()
    if not port:
        print("ERROR: No port specified and auto-detect failed.")
        sys.exit(1)

    print(f"Opening {port} at {args.baud} baud...")
    ser = serial.Serial(port, args.baud, timeout=1)
    time.sleep(2)

    # Drain boot messages
    while ser.in_waiting:
        line = ser.readline().decode('ascii', errors='replace').strip()
        if line:
            print(f"  < {line}")

    # Init
    send_cmd(ser, 'INIT')
    read_until_ok(ser)

    send_cmd(ser, 'ALGO T87')
    read_until_ok(ser)

    send_cmd(ser, 'SETID 7E2 7EA')
    read_until_ok(ser)

    # Start CALWRITE
    print("\n=== Starting T87 CALWRITE ===")
    send_cmd(ser, 'CALWRITE')

    # Wait for WRITE:READY
    vin = None
    deadline = time.time() + 120  # 2 min for setup + erase
    write_ready = False

    while time.time() < deadline:
        try:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode('ascii', errors='replace').strip()
        except Exception:
            continue
        if not line:
            continue
        print(f"  < {line}")

        if line.startswith('VIN:') and not line.startswith('VIN: ('):
            vin = line[4:].strip()

        if line.startswith('ERR:'):
            print(f"\n*** SETUP FAILED: {line} ***")
            ser.close()
            sys.exit(1)

        if line.startswith('WRITE:READY'):
            write_ready = True
            break

    if not write_ready:
        print("\n*** TIMEOUT waiting for WRITE:READY ***")
        ser.close()
        sys.exit(1)

    # Send WDATA blocks
    print(f"\n=== Sending {T87_CAL_BLOCKS} write blocks ===")
    start_time = time.time()

    for blk in range(T87_CAL_BLOCKS):
        addr = T87_CAL_START + blk * BLOCK_SIZE
        block = cal_data[blk * BLOCK_SIZE:(blk + 1) * BLOCK_SIZE]
        hex_data = block.hex().upper()

        wdata_line = f"WDATA:{addr:06X}:{hex_data}\r\n"
        ser.write(wdata_line.encode('ascii'))

        # Wait for ACK/NAK
        ack_deadline = time.time() + 30
        got_ack = False
        while time.time() < ack_deadline:
            try:
                raw = ser.readline()
                if not raw:
                    continue
                resp = raw.decode('ascii', errors='replace').strip()
            except Exception:
                continue
            if not resp:
                continue

            if resp.startswith('WDATA:ACK:'):
                got_ack = True
                break
            elif resp.startswith('WDATA:NAK:'):
                print(f"  < {resp}")
                print(f"\n*** WRITE FAILED at block {blk} addr=0x{addr:06X} ***")
                print("*** DO NOT POWER DOWN ***")
                ser.close()
                sys.exit(1)
            elif resp.startswith('WRITE:PROGRESS'):
                print(f"  < {resp}")
            elif resp.startswith('CRITICAL:') or resp.startswith('CALWRITE:TIMEOUT'):
                print(f"  < {resp}")
                print(f"\n*** WRITE FAILED ***")
                ser.close()
                sys.exit(1)
            else:
                # Print ALL firmware messages for debugging
                print(f"  < {resp}")

        if not got_ack:
            print(f"\n*** TIMEOUT waiting for ACK on block {blk} addr=0x{addr:06X} ***")
            print("*** DO NOT POWER DOWN ***")
            ser.close()
            sys.exit(1)

        # Progress
        if blk % 64 == 63 or blk == T87_CAL_BLOCKS - 1:
            elapsed = time.time() - start_time
            pct = (blk + 1) / T87_CAL_BLOCKS * 100
            rate = (blk + 1) * BLOCK_SIZE / elapsed / 1024 if elapsed > 0 else 0
            print(f"  Block {blk+1}/{T87_CAL_BLOCKS} ({pct:.0f}%) "
                  f"addr=0x{addr:06X} [{rate:.1f} KB/s]")

    write_elapsed = time.time() - start_time
    print(f"\nAll {T87_CAL_BLOCKS} blocks sent in {write_elapsed:.1f}s")

    # Wait for WRITE:DONE + finalize
    print("\n=== Waiting for finalize ===")
    finalize_deadline = time.time() + 30

    while time.time() < finalize_deadline:
        try:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode('ascii', errors='replace').strip()
        except Exception:
            continue
        if not line:
            continue
        print(f"  < {line}")

        if line.startswith('WRITE:DONE'):
            break
        if 'CRITICAL' in line or 'FAILED' in line:
            print("\n*** FINALIZE FAILED ***")
            print("*** DO NOT POWER DOWN ***")
            ser.close()
            sys.exit(1)

    # Wait for CALWRITE:DONE
    done_deadline = time.time() + 10
    while time.time() < done_deadline:
        try:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode('ascii', errors='replace').strip()
        except Exception:
            continue
        if not line:
            continue
        print(f"  < {line}")
        if 'CALWRITE:DONE' in line or line == 'OK':
            break

    total_elapsed = time.time() - start_time
    print(f"\nTotal time: {total_elapsed:.1f}s")
    if vin:
        print(f"VIN: {vin}")
    print("\nCalwrite complete. Verify with a separate FULLREAD if desired.")

    ser.close()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\n\nAborted by user.")
        print("*** If write was in progress, DO NOT POWER DOWN ***")
        sys.exit(1)
    except Exception as e:
        print(f"\n*** ERROR: {e} ***")
        import traceback
        traceback.print_exc()
        print("\n*** If write was in progress, DO NOT POWER DOWN ***")
        if getattr(sys, 'frozen', False):
            input("\nPress Enter to close...")
        sys.exit(1)
