#!/usr/bin/env python3
"""
test_write.py — external-kernel E38 incremental sector write test

Self-contained test: the Feather reads sector 0x1FE000 into RAM,
erases it, verifies erased, writes back from RAM, verifies checksum.

This script just orchestrates (INIT, ALGO, TESTWRITE) and monitors.
Optional: cross-validates checksums against backup .bin file.

Usage:
    python test_write.py [COM_PORT]
"""

import io
import os
import sys
import time
import serial

sys.path.insert(0, os.path.dirname(__file__))
from detect_port import find_feather_or_prompt

# Force UTF-8 output on Windows
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8',
                              errors='replace', line_buffering=True)

SECTOR_ADDR = 0x1FE000
SECTOR_SIZE = 0x2000  # 8KB


def compute_backup_checksum():
    """Compute expected checksum from backup .bin file for cross-validation."""
    base = os.path.dirname(os.path.dirname(__file__))
    backup = os.path.join(base, 'reads',
                          '2G1FK1EJ0A9118489_86AAMFK10055W6E9.bin')
    if not os.path.exists(backup):
        print(f"  (backup file not found: {backup})")
        return None
    with open(backup, 'rb') as f:
        data = f.read()
    if len(data) < SECTOR_ADDR + SECTOR_SIZE:
        print(f"  (backup file too small: {len(data)} bytes)")
        return None
    sector = data[SECTOR_ADDR:SECTOR_ADDR + SECTOR_SIZE]
    chk = sum(sector)
    print(f"  Backup checksum for 0x{SECTOR_ADDR:06X}: 0x{chk:X}")
    return chk


def send_cmd(ser, cmd, timeout=15):
    """Send command and read until OK or ERR."""
    print(f"  > {cmd}")
    ser.write((cmd + '\r\n').encode('ascii'))
    time.sleep(0.1)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            line = ser.readline().decode('ascii', errors='replace').strip()
        except Exception:
            continue
        if not line:
            continue
        print(f"  < {line}")
        if line == 'OK' or line.startswith('ERR:'):
            return line
    return None


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_feather_or_prompt()
    if not port:
        print("ERROR: No port specified and auto-detect failed.")
        sys.exit(1)

    # Cross-validate with backup file
    print("\n--- Backup file validation ---")
    expected_chk = compute_backup_checksum()

    print(f"\nOpening {port} at 115200 baud...")
    ser = serial.Serial(port, 115200, timeout=1)
    time.sleep(2)

    # Drain boot messages
    while ser.in_waiting:
        line = ser.readline().decode('ascii', errors='replace').strip()
        if line:
            print(f"  < {line}")

    # Init CAN
    print("\n--- Initializing ---")
    resp = send_cmd(ser, 'INIT')
    if resp and resp.startswith('ERR'):
        print("INIT failed — aborting")
        ser.close()
        sys.exit(1)

    # Set module type
    resp = send_cmd(ser, 'ALGO E38N')
    if resp and resp.startswith('ERR'):
        print("ALGO failed — aborting")
        ser.close()
        sys.exit(1)

    # Run TESTWRITE — monitor until completion
    print("\n--- Starting TESTWRITE ---")
    print(f"  > TESTWRITE")
    ser.write(b'TESTWRITE\r\n')

    checksums = {}
    deadline = time.time() + 300  # 5 minute timeout

    while time.time() < deadline:
        try:
            line = ser.readline().decode('ascii', errors='replace').strip()
        except Exception:
            continue
        if not line:
            continue
        print(f"  < {line}")

        # Capture checksums
        if line.startswith('TESTWRITE:CHECKSUM_ORIG'):
            val = line.split()[-1]
            checksums['orig'] = int(val, 16)
        elif line.startswith('TESTWRITE:CHECKSUM_ERASED'):
            val = line.split()[-1]
            checksums['erased'] = int(val, 16)
        elif line.startswith('TESTWRITE:CHECKSUM_FINAL'):
            val = line.split()[-1]
            checksums['final'] = int(val, 16)

        # Check for completion
        if line.startswith('TESTWRITE:DONE'):
            # Read trailing OK
            try:
                extra = ser.readline().decode('ascii', errors='replace').strip()
                if extra:
                    print(f"  < {extra}")
            except Exception:
                pass
            break

        # Critical errors
        if 'POWER DOWN' in line:
            print("\n  !!! CRITICAL — DO NOT POWER DOWN !!!")

    ser.close()

    # Summary
    print("\n" + "=" * 50)
    print("TESTWRITE SUMMARY")
    print("=" * 50)
    if 'orig' in checksums:
        print(f"  Original checksum:  0x{checksums['orig']:X}")
    if 'erased' in checksums:
        erased_ok = checksums['erased'] == 0xFF * SECTOR_SIZE
        print(f"  Erased checksum:    0x{checksums['erased']:X}"
              f"  {'(all 0xFF OK)' if erased_ok else '(NOT all 0xFF!)'}")
    if 'final' in checksums:
        match = checksums.get('final') == checksums.get('orig')
        print(f"  Final checksum:     0x{checksums['final']:X}"
              f"  {'MATCH' if match else 'MISMATCH!'}")
    if expected_chk is not None and 'orig' in checksums:
        backup_match = checksums['orig'] == expected_chk
        print(f"  Backup file match:  {'YES' if backup_match else 'NO'}"
              f"  (backup=0x{expected_chk:X})")
    print("=" * 50)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\nAborted by user.")
    except Exception as e:
        print(f"\n*** ERROR: {e} ***")
        import traceback
        traceback.print_exc()
        sys.exit(1)
