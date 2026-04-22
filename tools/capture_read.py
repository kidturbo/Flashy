#!/usr/bin/env python3
"""
capture_read.py — Capture ECU flash read from J2534 Pass-Thru Feather

Connects to the Feather serial port, sends FULLREAD (or manual commands),
captures DATA: lines, and saves the binary to a .bin file named from
the VIN and OSID read from the ECU.

Usage:
    python capture_read.py COM38                    # FULLREAD on E38 (default)
    python capture_read.py COM38 --module t87       # FULLREAD on T87
    python capture_read.py COM38 --manual           # Manual mode (interactive)
    python capture_read.py COM38 --output my.bin    # Override output filename

Requires: pip install pyserial
"""

import argparse
import datetime
import io
import os
import re
import serial
import sys
import time

from detect_port import find_feather_or_prompt

# Force UTF-8 output on Windows to handle special characters from firmware
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace', line_buffering=True)


def wait_for(ser, pattern, timeout=30):
    """Read lines until one matches pattern (regex). Returns match or None."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if ser.in_waiting or True:
            try:
                line = ser.readline().decode('ascii', errors='replace').strip()
            except Exception:
                continue
            if not line:
                continue
            print(f"  < {line}")
            m = re.search(pattern, line)
            if m:
                return m, line
    return None, None


def send_cmd(ser, cmd, echo=True):
    """Send a command and print it."""
    if echo:
        print(f"  > {cmd}")
    ser.write((cmd + '\r\n').encode('ascii'))
    time.sleep(0.1)


def read_until_ok_or_err(ser, timeout=30):
    """Read lines until OK or ERR, return all lines."""
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


def log_read_attempt(log_path, module, result, vin=None, osid=None, filename=None,
                     blocks=0, total_bytes=0, elapsed=0, error=None):
    """Append a read attempt entry to the log file."""
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    ts = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    with open(log_path, 'a', encoding='utf-8') as f:
        f.write(f"[{ts}] module={module.upper()} result={result}")
        if vin:
            f.write(f" VIN={vin}")
        if osid:
            f.write(f" OSID={osid}")
        if filename:
            f.write(f" file={filename}")
        if blocks:
            f.write(f" blocks={blocks}")
        if total_bytes:
            f.write(f" bytes={total_bytes}")
        if elapsed:
            f.write(f" elapsed={elapsed:.1f}s")
        if error:
            f.write(f" error={error}")
        f.write('\n')


def capture_fullread(ser, module, output_path, timeout_total=600):
    """Run FULLREAD and capture all data."""
    if getattr(sys, 'frozen', False):
        log_base = os.path.dirname(sys.executable)
    else:
        log_base = os.path.dirname(os.path.dirname(__file__))
    log_path = os.path.join(log_base, 'reads', 'read_log.txt')
    start_time = time.time()

    # Init CAN
    send_cmd(ser, 'INIT')
    read_until_ok_or_err(ser)

    # Set module/algo
    send_cmd(ser, f'ALGO {module.upper()}')
    read_until_ok_or_err(ser)

    # Set CAN IDs for T87 (TCM uses 0x7E2/0x7EA)
    if module.lower() == 't87':
        send_cmd(ser, 'SETID 7E2 7EA')
        read_until_ok_or_err(ser)

    # Run FULLREAD
    send_cmd(ser, 'FULLREAD')

    filename = output_path
    data_blocks = {}
    vin = None
    osid = None
    last_error = None
    deadline = time.time() + timeout_total

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

        # Don't flood console with DATA lines (just show progress)
        if line.startswith('DATA:'):
            parts = line.split(':', 2)
            if len(parts) == 3:
                addr_hex = parts[1]
                hex_data = parts[2]
                try:
                    addr = int(addr_hex, 16)
                    data_blocks[addr] = bytes.fromhex(hex_data)
                    block_num = addr // 0x800
                    if block_num % 64 == 0:
                        print(f"  < DATA block {block_num}  addr=0x{addr:06X}")
                except ValueError:
                    # Serial corruption — skip this block
                    print(f"  < WARN: corrupt DATA at {addr_hex} (len={len(hex_data)}), skipping")
            continue

        print(f"  < {line}")

        # Capture VIN/OSID for logging
        if line.startswith('VIN:') and not line.startswith('VIN: ('):
            vin = line[4:].strip()
        if line.startswith('OSID:') and not line.startswith('OSID: ('):
            osid = line[5:].strip()

        # Capture filename
        if line.startswith('FILE:'):
            auto_name = line[5:].strip()
            if not output_path:
                filename = auto_name

        # Track errors
        if line.startswith('ERR:'):
            last_error = line

        # Check for read progress
        if line.startswith('READ:PROGRESS'):
            pass  # already printed

        # Check for completion
        if line.startswith('READ:DONE') or line == 'OK':
            if line == 'OK' and not data_blocks:
                continue  # OK from an earlier sub-command
            if line.startswith('READ:DONE'):
                # Read one more line for OK
                try:
                    extra = ser.readline().decode('ascii', errors='replace').strip()
                    if extra:
                        print(f"  < {extra}")
                except Exception:
                    pass
                break

        # Check for fatal errors during read
        if line.startswith('ERR:') and data_blocks:
            print(f"\n*** Error during read at block {len(data_blocks)} ***")
            break

    elapsed = time.time() - start_time

    if not data_blocks:
        print("\nNo data captured!")
        log_read_attempt(log_path, module, 'FAIL', vin=vin, osid=osid,
                         elapsed=elapsed, error=last_error)
        return None

    # Assemble binary
    if not filename:
        filename = f"{module.upper()}_read.bin"

    # Sort by address and write
    sorted_addrs = sorted(data_blocks.keys())
    min_addr = sorted_addrs[0]
    max_addr = sorted_addrs[-1]
    block_size = len(data_blocks[sorted_addrs[0]])
    total_size = max_addr - min_addr + block_size

    print(f"\nAssembling {len(data_blocks)} blocks, {total_size} bytes "
          f"(0x{min_addr:06X} - 0x{max_addr + block_size - 1:06X})")

    # Build output in reads/ subdirectory (next to .exe or project root)
    if getattr(sys, 'frozen', False):
        base_dir = os.path.dirname(sys.executable)
    else:
        base_dir = os.path.dirname(os.path.dirname(__file__))
    output_dir = os.path.join(base_dir, 'reads')
    os.makedirs(output_dir, exist_ok=True)
    filepath = os.path.join(output_dir, filename)

    # If file already exists, append timestamp to avoid overwriting
    if os.path.exists(filepath):
        from datetime import datetime
        ts = datetime.now().strftime('%Y%m%d_%H%M%S')
        base, ext = os.path.splitext(filename)
        filename = f"{base}_{ts}{ext}"
        filepath = os.path.join(output_dir, filename)

    binary = bytearray(total_size)
    # Fill with 0xFF (unprogrammed flash)
    for i in range(total_size):
        binary[i] = 0xFF

    for addr, block in data_blocks.items():
        offset = addr - min_addr
        binary[offset:offset + len(block)] = block

    with open(filepath, 'wb') as f:
        f.write(binary)

    print(f"Saved: {filepath}")
    print(f"Size:  {len(binary)} bytes ({len(binary) / 1024 / 1024:.2f} MB)")

    log_read_attempt(log_path, module, 'OK', vin=vin, osid=osid,
                     filename=filename, blocks=len(data_blocks),
                     total_bytes=len(binary), elapsed=elapsed)
    return filepath


def capture_calread(ser, module, output_path, timeout_total=300):
    """Run CALREAD and capture calibration-only data."""
    if module.lower() not in ('e38', 't87'):
        print("ERROR: CALREAD only supported for E38/T87 currently.")
        return None

    if getattr(sys, 'frozen', False):
        log_base = os.path.dirname(sys.executable)
    else:
        log_base = os.path.dirname(os.path.dirname(__file__))
    log_path = os.path.join(log_base, 'reads', 'read_log.txt')
    start_time = time.time()

    # Init CAN
    send_cmd(ser, 'INIT')
    read_until_ok_or_err(ser)

    # Set module/algo
    send_cmd(ser, f'ALGO {module.upper()}')
    read_until_ok_or_err(ser)

    # Set CAN IDs for T87 (TCM uses 0x7E2/0x7EA)
    if module.lower() == 't87':
        send_cmd(ser, 'SETID 7E2 7EA')
        read_until_ok_or_err(ser)

    # Run CALREAD
    send_cmd(ser, 'CALREAD')

    # Reuse same capture logic as FULLREAD
    filename = output_path
    data_blocks = {}
    vin = None
    osid = None
    last_error = None
    deadline = time.time() + timeout_total

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

        if line.startswith('DATA:'):
            parts = line.split(':', 2)
            if len(parts) == 3:
                addr_hex = parts[1]
                hex_data = parts[2]
                try:
                    addr = int(addr_hex, 16)
                    data_blocks[addr] = bytes.fromhex(hex_data)
                    # Calculate block number relative to first block seen
                    if not data_blocks or addr == min(data_blocks.keys()):
                        cal_base = addr
                    else:
                        cal_base = min(data_blocks.keys())
                    block_num = (addr - cal_base) // 0x800
                    total_blocks = 512 if module.lower() == 't87' else 128
                    if block_num % 16 == 0:
                        print(f"  < DATA block {block_num}/{total_blocks}  addr=0x{addr:06X}")
                except ValueError:
                    print(f"  < WARN: corrupt DATA at {addr_hex}, skipping")
            continue

        print(f"  < {line}")

        if line.startswith('VIN:') and not line.startswith('VIN: ('):
            vin = line[4:].strip()
        if line.startswith('OSID:') and not line.startswith('OSID: ('):
            osid = line[5:].strip()
        if line.startswith('FILE:'):
            auto_name = line[5:].strip()
            if not output_path:
                filename = auto_name
        if line.startswith('ERR:'):
            last_error = line
        if line.startswith('READ:DONE') or (line == 'OK' and data_blocks):
            if line.startswith('READ:DONE'):
                try:
                    extra = ser.readline().decode('ascii', errors='replace').strip()
                    if extra:
                        print(f"  < {extra}")
                except Exception:
                    pass
            break
        if line.startswith('ERR:') and data_blocks:
            print(f"\n*** Error during cal read at block {len(data_blocks)} ***")
            break

    elapsed = time.time() - start_time

    if not data_blocks:
        print("\nNo calibration data captured!")
        log_read_attempt(log_path, module, 'FAIL-CAL', vin=vin, osid=osid,
                         elapsed=elapsed, error=last_error)
        return None

    if not filename:
        filename = f"{module.upper()}_cal_read.bin"

    sorted_addrs = sorted(data_blocks.keys())
    min_addr = sorted_addrs[0]
    max_addr = sorted_addrs[-1]
    block_size = len(data_blocks[sorted_addrs[0]])
    total_size = max_addr - min_addr + block_size

    print(f"\nAssembling {len(data_blocks)} cal blocks, {total_size} bytes "
          f"(0x{min_addr:06X} - 0x{max_addr + block_size - 1:06X})")

    if getattr(sys, 'frozen', False):
        base_dir = os.path.dirname(sys.executable)
    else:
        base_dir = os.path.dirname(os.path.dirname(__file__))
    output_dir = os.path.join(base_dir, 'reads')
    os.makedirs(output_dir, exist_ok=True)
    filepath = os.path.join(output_dir, filename)

    # If file already exists, append timestamp to avoid overwriting
    if os.path.exists(filepath):
        from datetime import datetime
        ts = datetime.now().strftime('%Y%m%d_%H%M%S')
        base, ext = os.path.splitext(filename)
        filename = f"{base}_{ts}{ext}"
        filepath = os.path.join(output_dir, filename)

    binary = bytearray(b'\xFF' * total_size)
    for addr, block in data_blocks.items():
        offset = addr - min_addr
        binary[offset:offset + len(block)] = block

    with open(filepath, 'wb') as f:
        f.write(binary)

    print(f"Saved: {filepath}")
    print(f"Size:  {len(binary)} bytes ({len(binary) / 1024:.0f} KB)")

    log_read_attempt(log_path, module, 'OK-CAL', vin=vin, osid=osid,
                     filename=filename, blocks=len(data_blocks),
                     total_bytes=len(binary), elapsed=elapsed)
    return filepath


def manual_mode(ser):
    """Interactive mode — type commands, capture DATA lines."""
    print("Manual mode. Type commands, Ctrl+C to quit.")
    print("Use 'SAVE <filename>' to save captured data.\n")

    data_blocks = {}

    import threading

    def reader():
        while True:
            try:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode('ascii', errors='replace').strip()
                if not line:
                    continue
                if line.startswith('DATA:'):
                    parts = line.split(':', 2)
                    if len(parts) == 3:
                        addr = int(parts[1], 16)
                        data_blocks[addr] = bytes.fromhex(parts[2])
                        if (addr // 0x800) % 64 == 0:
                            print(f"  < DATA block {addr // 0x800}")
                    continue
                print(f"  < {line}")
            except Exception:
                break

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    try:
        while True:
            cmd = input("> ").strip()
            if cmd.upper().startswith('SAVE '):
                fname = cmd[5:].strip()
                if data_blocks:
                    sorted_addrs = sorted(data_blocks.keys())
                    min_a = sorted_addrs[0]
                    max_a = sorted_addrs[-1]
                    bsz = len(data_blocks[sorted_addrs[0]])
                    total = max_a - min_a + bsz
                    binary = bytearray(b'\xFF' * total)
                    for a, d in data_blocks.items():
                        off = a - min_a
                        binary[off:off + len(d)] = d
                    with open(fname, 'wb') as f:
                        f.write(binary)
                    print(f"Saved {len(binary)} bytes to {fname}")
                else:
                    print("No data captured yet.")
                continue
            send_cmd(ser, cmd, echo=False)
    except KeyboardInterrupt:
        print(f"\nCaptured {len(data_blocks)} blocks.")


def main():
    parser = argparse.ArgumentParser(description='Capture ECU read from J2534 Pass-Thru')
    parser.add_argument('port', nargs='?', default=None,
                        help='Serial port (e.g. COM38) — auto-detected if omitted')
    parser.add_argument('--baud', type=int, default=115200, help='Baud rate (default 115200)')
    parser.add_argument('--module', default='e38', help='Module type: e38, t87, t42, e92')
    parser.add_argument('--output', default=None, help='Output filename (overrides auto-naming)')
    parser.add_argument('--cal', action='store_true', help='Calibration-only read (E38: 256KB)')
    parser.add_argument('--manual', action='store_true', help='Interactive manual mode')
    parser.add_argument('--timeout', type=int, default=600, help='Total timeout seconds (default 600)')
    args = parser.parse_args()

    port = args.port or find_feather_or_prompt()
    if not port:
        print("ERROR: No port specified and auto-detect failed.")
        sys.exit(1)

    print(f"Opening {port} at {args.baud} baud...")
    ser = serial.Serial(port, args.baud, timeout=1)
    time.sleep(2)  # wait for Feather reset

    # Drain any boot messages
    while ser.in_waiting:
        line = ser.readline().decode('ascii', errors='replace').strip()
        if line:
            print(f"  < {line}")

    if args.manual:
        manual_mode(ser)
    elif args.cal:
        capture_calread(ser, args.module, args.output, args.timeout)
    else:
        capture_fullread(ser, args.module, args.output, args.timeout)

    ser.close()


if __name__ == '__main__':
    try:
        main()
    except Exception as e:
        print(f"\n*** ERROR: {e} ***")
        import traceback
        traceback.print_exc()
        if getattr(sys, 'frozen', False):
            input("\nPress Enter to close...")
        sys.exit(1)
