#!/usr/bin/env python3
"""
t87_fullwrite.py — T87 TCM full flash write via J2534 Pass-Thru Feather

Loads a 4MB .bin file, extracts the write region (0x080000-0x3FFFFF, 3.5MB),
sends it to the Feather which writes it to the TCM using the
full flash kernel protocol ($36 EE erase, $36 00 write blocks, $36 FF finalize).

This writes both calibration AND OS code. Header (0x000000-0x007FFF) and
NVM/Adaptation (0x008000-0x07FFFF) are NOT written.

After writing, captures the readback and verifies it matches.

Usage:
    python t87_fullwrite.py COM38 firmware.bin
    python t87_fullwrite.py --help

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

T87_WRITE_START  = 0x080000
T87_WRITE_SIZE   = 0x380000   # 3.5 MB (0x080000 to 0x3FFFFF)
T87_WRITE_BLOCKS = 1792       # 0x380000 / 0x800
BLOCK_SIZE       = 0x800      # 2048 bytes


def send_cmd(ser, cmd):
    print(f"  > {cmd}")
    ser.write((cmd + '\r\n').encode('ascii'))
    time.sleep(0.1)


def read_lines_until(ser, pattern, timeout=30):
    """Read lines until pattern matches."""
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
    parser = argparse.ArgumentParser(description='T87 TCM full flash write')
    parser.add_argument('port', nargs='?', default=None,
                        help='Serial port (auto-detected if omitted)')
    parser.add_argument('binfile', help='4MB .bin file to write')
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--timeout', type=int, default=1200,
                        help='Total timeout seconds (default 1200)')
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

    if len(full_bin) < T87_WRITE_START + T87_WRITE_SIZE:
        print(f"ERROR: File too small. Need at least {T87_WRITE_START + T87_WRITE_SIZE} bytes "
              f"(got {len(full_bin)})")
        sys.exit(1)

    write_data = full_bin[T87_WRITE_START:T87_WRITE_START + T87_WRITE_SIZE]
    print(f"Write region: 0x{T87_WRITE_START:06X}-0x{T87_WRITE_START + T87_WRITE_SIZE - 1:06X} "
          f"({len(write_data)} bytes, {T87_WRITE_BLOCKS} blocks, "
          f"{len(write_data)/1024/1024:.1f} MB)")

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

    # Start FULLWRITE
    print("\n=== Starting T87 FULLWRITE ===")
    print("*** WARNING: This writes calibration AND OS code ***")
    print("*** Header and NVM/Adaptation regions are NOT modified ***\n")
    send_cmd(ser, 'FULLWRITE')

    # Wait for WRITE:READY (erase takes ~40s for full flash)
    vin = None
    deadline = time.time() + 180  # 3 min for setup + full erase
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
    print(f"\n=== Sending {T87_WRITE_BLOCKS} write blocks ({len(write_data)/1024/1024:.1f} MB) ===")
    start_time = time.time()

    for blk in range(T87_WRITE_BLOCKS):
        addr = T87_WRITE_START + blk * BLOCK_SIZE
        block = write_data[blk * BLOCK_SIZE:(blk + 1) * BLOCK_SIZE]
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
            elif resp.startswith('CRITICAL:') or resp.startswith('FULLWRITE:TIMEOUT'):
                print(f"  < {resp}")
                print(f"\n*** WRITE FAILED ***")
                ser.close()
                sys.exit(1)

        if not got_ack:
            print(f"\n*** TIMEOUT waiting for ACK on block {blk} addr=0x{addr:06X} ***")
            print("*** DO NOT POWER DOWN ***")
            ser.close()
            sys.exit(1)

        # Progress every 64 blocks (~128KB)
        if blk % 64 == 63 or blk == T87_WRITE_BLOCKS - 1:
            elapsed = time.time() - start_time
            pct = (blk + 1) / T87_WRITE_BLOCKS * 100
            rate = (blk + 1) * BLOCK_SIZE / elapsed / 1024 if elapsed > 0 else 0
            print(f"  Block {blk+1}/{T87_WRITE_BLOCKS} ({pct:.0f}%) "
                  f"addr=0x{addr:06X} [{rate:.1f} KB/s]")

    write_elapsed = time.time() - start_time
    print(f"\nAll {T87_WRITE_BLOCKS} blocks sent in {write_elapsed:.1f}s")

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

    # Wait for FULLWRITE:DONE
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
        if 'FULLWRITE:DONE' in line or line == 'OK':
            break

    total_elapsed = time.time() - start_time
    print(f"\nTotal time: {total_elapsed:.1f}s")
    if vin:
        print(f"VIN: {vin}")
    print("\nFullwrite complete. Verify with a separate FULLREAD if desired.")

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
