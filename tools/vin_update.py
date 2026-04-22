#!/usr/bin/env python3
"""
vin_update.py — Update VIN on a GM ECU via J2534 Pass-Thru Feather

Connects to the Feather, enters programming mode, writes the new VIN,
then returns to normal.

Usage:
    python vin_update.py COM38 2G1FK1EJ0A9118400 --module e38
    python vin_update.py COM38 --read              # just read current VIN

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
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'gm5byte'))

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace',
                              line_buffering=True)


def send_cmd(ser, cmd, echo=True):
    if echo:
        print(f"  > {cmd}")
    ser.write((cmd + '\r\n').encode('ascii'))
    time.sleep(0.1)


def read_until_ok_or_err(ser, timeout=10):
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


def find_in_lines(lines, prefix):
    for line in lines:
        # Strip leading "> " prompt if present
        clean = line.lstrip('> ').strip()
        if clean.startswith(prefix):
            return clean[len(prefix):].strip()
    return None


SUPPORTED_MODULES = [
    ('e38',  'E38 ECM',  '0x7E0/0x7E8', 402),
    ('e67',  'E67 ECM',  '0x7E0/0x7E8', 393),
    ('e92',  'E92 ECM',  '0x7E0/0x7E8', 513),
    ('t87',  'T87 TCM',  '0x7E2/0x7EA', 569),
    ('t87a', 'T87A TCM', '0x7E2/0x7EA', 569),
    ('t93',  'T93 TCM',  '0x7E2/0x7EA',  93),
    ('t42',  'T42 TCM',  '0x7E2/0x7EA', 371),
]


def prompt_module():
    """Interactive picker — every module has different CAN IDs and a
    different seed-key algorithm, so we MUST ask before writing."""
    print()
    print('Pick the module you are talking to:')
    print('  ' + '-' * 50)
    for i, (key, label, ids, algo) in enumerate(SUPPORTED_MODULES, 1):
        print(f'  {i}) {label:<10}  CAN {ids}   algo {algo}')
    print('  ' + '-' * 50)
    while True:
        choice = input('  Choice [1]: ').strip() or '1'
        if choice.isdigit() and 1 <= int(choice) <= len(SUPPORTED_MODULES):
            picked = SUPPORTED_MODULES[int(choice) - 1]
            print(f'  -> {picked[1]} (algo {picked[3]})')
            return picked[0]
        print('  invalid — pick a number from the list')


def main():
    parser = argparse.ArgumentParser(description='Update VIN on a GM ECM/TCM')
    parser.add_argument('port', nargs='?', default=None,
                        help='Serial port (e.g. COM38) — auto-detected if omitted')
    parser.add_argument('vin', nargs='?', default=None,
                        help='New 17-character VIN to write')
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--module', default=None,
                        help='Module type (e38/e67/e92/t87/t87a/t93/t42). '
                             'Prompts interactively if omitted.')
    parser.add_argument('--read', action='store_true',
                        help='Just read and display current VIN')
    args = parser.parse_args()

    if args.vin and len(args.vin) != 17:
        parser.error(f"VIN must be exactly 17 characters (got {len(args.vin)})")

    # No --module on the command line? Ask the user. Different modules
    # have different CAN IDs and seed-key algos, so guessing here would
    # silently send commands to the wrong place.
    if not args.module:
        args.module = prompt_module()
    else:
        valid = {m[0] for m in SUPPORTED_MODULES}
        if args.module.lower() not in valid:
            parser.error(f"Unknown --module '{args.module}'. Valid: {sorted(valid)}")
        args.module = args.module.lower()

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

    # Init CAN
    send_cmd(ser, 'INIT')
    read_until_ok_or_err(ser)

    # Set module
    send_cmd(ser, f'ALGO {args.module.upper()}')
    read_until_ok_or_err(ser)

    # Read current VIN
    send_cmd(ser, 'VIN')
    lines = read_until_ok_or_err(ser)
    current_vin = find_in_lines(lines, 'VIN:')
    if current_vin:
        print(f"\n  Current VIN: {current_vin}")
    else:
        print("\n  Could not read current VIN")

    if args.read:
        ser.close()
        return

    # Prompt for new VIN if not provided on command line
    new_vin = args.vin
    if not new_vin:
        print()
        new_vin = input("  Enter new 17-character VIN: ").strip().upper()
        if len(new_vin) != 17:
            print(f"  ERROR: VIN must be exactly 17 characters (got {len(new_vin)})")
            ser.close()
            return

    print(f"  New VIN:     {new_vin}")
    print()

    # Confirm
    confirm = input("  Proceed with VIN update? (y/yes): ").strip().lower()
    if confirm not in ('y', 'yes'):
        print("  Cancelled.")
        ser.close()
        return

    print()

    # SecurityAccess
    print("  Authenticating...")
    send_cmd(ser, 'AUTH')
    lines = read_until_ok_or_err(ser, timeout=5)

    if any('unlocked' in l.lower() for l in lines):
        pass  # 2-byte seed auto-computed — already unlocked
    elif any('provide key' in l.lower() for l in lines):
        # 5-byte seed — compute key on PC side
        seed_hex = None
        for line in lines:
            clean = line.lstrip('> ').strip()
            if clean.startswith('SEED:'):
                seed_hex = clean[5:].strip().replace(' ', '')
                break
        if not seed_hex or len(seed_hex) != 10:
            print(f"\n*** Could not parse 5-byte seed (got '{seed_hex}') ***")
            ser.close()
            return
        seed_bytes = bytes.fromhex(seed_hex)
        print(f"  5-byte seed: {seed_hex.upper()}")

        # 5-byte SecurityAccess algorithm IDs by module. E67 and T93
        # use the 2-byte path so they don't appear here — if the
        # firmware ever returns a 5-byte seed for them, the 0x87
        # default below will fail safely on the AUTH check.
        GM_5BYTE_ALGO = {
            'E38':  0x39,
            'E92':  0x39,
            'T87':  0x87,
            'T87A': 0x87,
            'T42':  0x87,
        }
        algo_id = GM_5BYTE_ALGO.get(args.module.upper(), 0x87)
        try:
            from keylib import derive_key_from_algo
            mac, _, _ = derive_key_from_algo(algo_id, seed_bytes)
            key_hex = mac.hex().upper()
            print(f"  Computed key: {key_hex} (algo=0x{algo_id:02X})")
        except Exception as e:
            print(f"\n*** 5-byte key computation failed: {e} ***")
            ser.close()
            return

        send_cmd(ser, f'AUTH {key_hex}')
        lines = read_until_ok_or_err(ser, timeout=5)
        if not any('unlocked' in l.lower() for l in lines):
            print("\n*** Security access failed (key rejected) ***")
            ser.close()
            return
    else:
        print("\n*** Security access failed ***")
        ser.close()
        return

    # Enter programming session
    print("  Entering programming session...")
    if args.module.upper() == 'E38':
        # E38: use broadcast programming mode entry
        # disableDTCSetting first
        send_cmd(ser, 'RAW A2')
        read_until_ok_or_err(ser, timeout=3)

        # Broadcast programming sequence via CANSEND
        print("  Broadcasting programming mode...")
        send_cmd(ser, 'CANSEND 101 FE 02 10 02 AA AA AA AA')
        read_until_ok_or_err(ser, timeout=2)
        time.sleep(0.5)

        send_cmd(ser, 'CANSEND 101 FE 01 28 AA AA AA AA AA')
        read_until_ok_or_err(ser, timeout=2)
        time.sleep(0.1)

        send_cmd(ser, 'CANSEND 101 FE 01 A2 AA AA AA AA AA')
        read_until_ok_or_err(ser, timeout=2)
        time.sleep(0.5)
    else:
        # Other modules: standard DiagSession programming
        send_cmd(ser, 'DIAG 2')
        lines = read_until_ok_or_err(ser, timeout=5)
        if not any('active' in l.lower() for l in lines):
            print("\n*** Failed to enter programming session ***")
            ser.close()
            return

    # Write VIN
    print(f"  Writing VIN: {new_vin}")
    send_cmd(ser, f'VINWRITE {new_vin}')
    lines = read_until_ok_or_err(ser, timeout=10)

    if any('OK' == l for l in lines):
        print(f"\n  VIN updated successfully to: {new_vin}")
    else:
        print("\n  VIN write may have failed. Check output above.")

    # Verify by reading back
    print("\n  Verifying...")
    send_cmd(ser, 'VIN')
    lines = read_until_ok_or_err(ser)
    readback = find_in_lines(lines, 'VIN:')
    if readback:
        print(f"  Read-back VIN: {readback}")
        if readback == new_vin:
            print("  VERIFIED OK")
        else:
            print(f"  WARNING: mismatch! Expected {new_vin}")

    # Return to normal if E38
    if args.module.upper() == 'E38':
        send_cmd(ser, 'CANSEND 101 FE 01 20 AA AA AA AA AA')
        read_until_ok_or_err(ser, timeout=2)

    ser.close()
    print("\nDone.")


if __name__ == '__main__':
    main()
