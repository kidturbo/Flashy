#!/usr/bin/env python3
"""
vin_scan.py — Scan CAN bus for all modules, display VINs, update any module's VIN.

Connects to the Feather, runs SCAN to find all modules, reads VIN from each,
then lets the user choose which module's VIN to update.

Usage:
    python vin_scan.py                        # auto-detect COM port
    python vin_scan.py COM38                  # specify port
    python vin_scan.py --scan-only            # just scan, no update

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
        clean = line.lstrip('> ').strip()
        if clean.startswith(prefix):
            return clean[len(prefix):].strip()
    return None


def parse_scan_results(lines):
    """Parse SCAN output into a list of {tx_id, rx_id, name, vin}."""
    modules = []
    current = None
    for line in lines:
        clean = line.lstrip('> ').strip()
        # SCAN:FOUND 0x7E0/0x7E8 ECM
        m = re.match(r'SCAN:FOUND\s+0x([0-9A-Fa-f]+)/0x([0-9A-Fa-f]+)\s+(\S+)', clean)
        if m:
            current = {
                'tx_id': m.group(1),
                'rx_id': m.group(2),
                'name': m.group(3),
                'vin': None,
            }
            modules.append(current)
            continue
        # SCAN:VIN 0x7E0 2G1FK1EJ0A9118495
        m = re.match(r'SCAN:VIN\s+0x([0-9A-Fa-f]+)\s+(\S+)', clean)
        if m:
            tx = m.group(1)
            vin = m.group(2)
            for mod in modules:
                if mod['tx_id'].upper() == tx.upper():
                    mod['vin'] = vin
    return modules


def print_module_table(modules):
    print()
    print("  ╔════╦════════╦════════╦══════╦═══════════════════╗")
    print("  ║  # ║  TX ID ║  RX ID ║ Name ║       VIN         ║")
    print("  ╠════╬════════╬════════╬══════╬═══════════════════╣")
    for i, mod in enumerate(modules):
        vin = mod['vin'] or '(not available)'
        print(f"  ║ {i+1:2} ║ 0x{mod['tx_id']:>4s} ║ 0x{mod['rx_id']:>4s} ║ {mod['name']:<4s} ║ {vin:<17s} ║")
    print("  ╚════╩════════╩════════╩══════╩═══════════════════╝")
    print()


def update_vin_for_module(ser, mod):
    """Update VIN for a specific module."""
    tx_id = mod['tx_id']
    rx_id = mod['rx_id']
    name = mod['name']

    print(f"\n  Selected: {name} (0x{tx_id}/0x{rx_id})")
    if mod['vin']:
        print(f"  Current VIN: {mod['vin']}")

    new_vin = input("  Enter new 17-character VIN: ").strip().upper()
    if len(new_vin) != 17:
        print(f"  ERROR: VIN must be exactly 17 characters (got {len(new_vin)})")
        return False

    print(f"  New VIN:     {new_vin}")
    confirm = input("  Proceed? (y/yes): ").strip().lower()
    if confirm not in ('y', 'yes'):
        print("  Cancelled.")
        return False

    # Switch to this module's CAN IDs
    send_cmd(ser, f'SETID {tx_id} {rx_id}')
    read_until_ok_or_err(ser, timeout=3)

    # Determine module type for algo/programming mode
    # ECM addresses (0x7E0, 0x7E1) likely E38-type
    # TCM addresses (0x7E2) likely T87-type
    is_ecm = tx_id.upper() in ('7E0', '7E1')

    if is_ecm:
        send_cmd(ser, 'ALGO E38')
    else:
        send_cmd(ser, 'ALGO T87')
    read_until_ok_or_err(ser, timeout=3)

    # SecurityAccess
    print("\n  Authenticating...")
    send_cmd(ser, 'AUTH')
    lines = read_until_ok_or_err(ser, timeout=5)

    if any('unlocked' in l.lower() for l in lines):
        pass  # 2-byte seed auto-computed by firmware — already unlocked
    elif any('provide key' in l.lower() for l in lines):
        # 5-byte seed — compute key on PC side
        seed_hex = None
        for line in lines:
            clean = line.lstrip('> ').strip()
            if clean.startswith('SEED:'):
                seed_hex = clean[5:].strip().replace(' ', '')
                break
        if not seed_hex or len(seed_hex) != 10:
            print(f"\n  *** Could not parse 5-byte seed (got '{seed_hex}') ***")
            return False
        seed_bytes = bytes.fromhex(seed_hex)
        print(f"  5-byte seed: {seed_hex.upper()}")

        # GM 5-byte key derivation (AES-128 + SHA-256)
        # T87A = algo 0x87, mapped by module type
        GM_5BYTE_ALGO = {
            'TCM': 0x87,
            'ECM': 0x39,  # fallback, may vary
        }
        algo_id = GM_5BYTE_ALGO.get(name, 0x87)
        try:
            from keylib import derive_key_from_algo
            mac, iters, aes_key = derive_key_from_algo(algo_id, seed_bytes)
            key_hex = mac.hex().upper()
            print(f"  Computed key: {key_hex} (algo=0x{algo_id:02X}, iters={iters})")
        except Exception as e:
            print(f"\n  *** 5-byte key computation failed: {e} ***")
            return False

        # Send computed key back to Feather
        send_cmd(ser, f'AUTH {key_hex}')
        lines = read_until_ok_or_err(ser, timeout=5)
        if not any('unlocked' in l.lower() for l in lines):
            print("\n  *** Security access failed (key rejected) ***")
            return False
    else:
        print("\n  *** Security access failed ***")
        return False

    # Enter programming session
    print("  Entering programming session...")
    if is_ecm:
        send_cmd(ser, 'RAW A2')
        read_until_ok_or_err(ser, timeout=3)

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
        send_cmd(ser, 'DIAG 2')
        lines = read_until_ok_or_err(ser, timeout=5)

    # Write VIN
    print(f"  Writing VIN: {new_vin}")
    send_cmd(ser, f'VINWRITE {new_vin}')
    lines = read_until_ok_or_err(ser, timeout=10)

    if any('OK' == l for l in lines):
        print(f"\n  VIN updated successfully to: {new_vin}")
    else:
        print("\n  VIN write may have failed. Check output above.")

    # Verify
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

    # Return to normal if ECM
    if is_ecm:
        send_cmd(ser, 'CANSEND 101 FE 01 20 AA AA AA AA AA')
        read_until_ok_or_err(ser, timeout=2)

    return True


def main():
    parser = argparse.ArgumentParser(description='Scan CAN bus for modules and update VINs')
    parser.add_argument('port', nargs='?', default=None,
                        help='Serial port (auto-detected if omitted)')
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--scan-only', action='store_true',
                        help='Just scan, do not offer VIN update')
    args = parser.parse_args()

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

    # Run SCAN
    print("\n  Scanning CAN bus for modules...\n")
    send_cmd(ser, 'SCAN')
    lines = read_until_ok_or_err(ser, timeout=30)

    modules = parse_scan_results(lines)

    if not modules:
        print("\n  No modules found on the bus.")
        ser.close()
        return

    print_module_table(modules)

    if args.scan_only:
        ser.close()
        print("Done.")
        return

    # Let user pick a module to update
    while True:
        choice = input(f"  Enter module # to update VIN (1-{len(modules)}), or 'q' to quit: ").strip()
        if choice.lower() in ('q', 'quit', ''):
            break
        try:
            idx = int(choice) - 1
            if 0 <= idx < len(modules):
                update_vin_for_module(ser, modules[idx])
            else:
                print(f"  Invalid choice. Enter 1-{len(modules)}.")
        except ValueError:
            print(f"  Invalid choice. Enter 1-{len(modules)} or 'q'.")

    ser.close()
    print("\nDone.")


if __name__ == '__main__':
    main()
