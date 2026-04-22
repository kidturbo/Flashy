#!/usr/bin/env python3
"""
capture_write.py — Write .bin firmware to ECU via J2534 Pass-Thru Feather

Connects to the Feather serial port, sends CALWRITE or FULLWRITE,
streams .bin data block-by-block, then captures readback for verification.

Usage:
    python capture_write.py firmware.bin                    # CALWRITE E38 (default)
    python capture_write.py firmware.bin --full             # FULLWRITE E38
    python capture_write.py firmware.bin --module t87 --full  # FULLWRITE T87
    python capture_write.py firmware.bin --no-verify        # Skip readback verify
    python capture_write.py firmware.bin --verify-only      # Readback compare only

Requires: pip install pyserial
"""

import argparse
import datetime
import hashlib
import io
import os
import re
import serial
import struct
import sys
import time
import zlib

from detect_port import find_feather_or_prompt

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace',
                              line_buffering=True)

BLOCK_SIZE = 0x800  # 2048 bytes per block

# Expected sizes by module
MODULE_SIZES = {
    'e38': {'full': 0x200000, 'cal': 0x040000},   # 2MB / 256KB
    't87': {'full': 0x400000, 'cal': None},
    't42': {'full': None,     'cal': None},
    'e92': {'full': None,     'cal': None},
}


def send_cmd(ser, cmd, echo=True):
    if echo:
        print(f"  > {cmd}")
    ser.write((cmd + '\r\n').encode('ascii'))
    time.sleep(0.1)


def read_until_ok_or_err(ser, timeout=30):
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


def wait_for(ser, pattern, timeout=30, echo=True):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            line = ser.readline().decode('ascii', errors='replace').strip()
        except Exception:
            continue
        if not line:
            continue
        if echo:
            print(f"  < {line}")
        m = re.search(pattern, line)
        if m:
            return m, line
    return None, None


def log_write_attempt(log_path, module, result, vin=None, osid=None, filename=None,
                      blocks=0, total_bytes=0, elapsed=0, error=None):
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


def validate_bin_file(filepath, module, is_cal):
    """Pre-flight validation of .bin file."""
    if not os.path.isfile(filepath):
        print(f"ERROR: File not found: {filepath}")
        return None

    data = open(filepath, 'rb').read()
    size = len(data)
    mode = 'cal' if is_cal else 'full'
    expected = MODULE_SIZES.get(module.lower(), {}).get(mode)

    if expected and size != expected:
        print(f"ERROR: File size {size} bytes does not match expected "
              f"{expected} bytes for {module.upper()} {mode} write.")
        print(f"  Expected: {expected:,} bytes ({expected // 1024} KB)")
        print(f"  Got:      {size:,} bytes ({size // 1024} KB)")
        return None

    # Reject obviously corrupt files
    if data == b'\xFF' * size:
        print("ERROR: File is all 0xFF (blank/erased). Refusing to write.")
        return None
    if data == b'\x00' * size:
        print("ERROR: File is all 0x00. Refusing to write.")
        return None

    # Compute checksums
    crc32 = zlib.crc32(data) & 0xFFFFFFFF
    md5 = hashlib.md5(data).hexdigest()

    print(f"\n  File:   {os.path.basename(filepath)}")
    print(f"  Size:   {size:,} bytes ({size // 1024} KB)")
    print(f"  CRC32:  {crc32:08X}")
    print(f"  MD5:    {md5}")
    print(f"  Blocks: {size // BLOCK_SIZE}")

    return data


def do_write(ser, bin_data, module, is_cal, no_verify, timeout_total=1200):
    """Execute the write operation."""
    if getattr(sys, 'frozen', False):
        log_base = os.path.dirname(sys.executable)
    else:
        log_base = os.path.dirname(os.path.dirname(__file__))
    log_path = os.path.join(log_base, 'writes', 'write_log.txt')
    start_time = time.time()

    write_cmd = 'CALWRITE' if is_cal else 'FULLWRITE'

    # Init CAN
    send_cmd(ser, 'INIT')
    read_until_ok_or_err(ser)

    send_cmd(ser, f'ALGO {module.upper()}')
    read_until_ok_or_err(ser)

    # Send write command
    send_cmd(ser, write_cmd)

    # Read lines until we see WRITE:READY (auth, kernel upload, erase happening)
    vin = None
    osid = None
    write_ready = False
    start_addr = 0
    num_blocks = 0
    deadline = time.time() + 120  # 2 min for setup + erase

    while time.time() < deadline:
        try:
            line = ser.readline().decode('ascii', errors='replace').strip()
        except Exception:
            continue
        if not line:
            continue
        print(f"  < {line}")

        if line.startswith('VIN:') and not line.startswith('VIN: ('):
            vin = line[4:].strip()
        if line.startswith('OSID:') and not line.startswith('OSID: ('):
            osid = line[5:].strip()

        if line.startswith('ERR:') or line.startswith('CRITICAL:'):
            print(f"\n*** SETUP FAILED: {line} ***")
            log_write_attempt(log_path, module, 'FAIL-SETUP', vin=vin, osid=osid,
                              error=line, elapsed=time.time() - start_time)
            return False

        m = re.match(r'WRITE:READY\s+([0-9A-Fa-f]+)\s+(\d+)', line)
        if m:
            start_addr = int(m.group(1), 16)
            num_blocks = int(m.group(2))
            write_ready = True
            break

    if not write_ready:
        print("\n*** Timeout waiting for WRITE:READY ***")
        log_write_attempt(log_path, module, 'FAIL-TIMEOUT', vin=vin, osid=osid,
                          elapsed=time.time() - start_time)
        return False

    total_blocks = len(bin_data) // BLOCK_SIZE
    if num_blocks != total_blocks:
        print(f"WARNING: Feather expects {num_blocks} blocks, file has {total_blocks}")
        num_blocks = min(num_blocks, total_blocks)

    # ===== WRITE PHASE =====
    print(f"\n{'='*60}")
    print(f"  WARNING: DO NOT power off vehicle or disconnect USB")
    print(f"  until verification is complete!")
    print(f"{'='*60}")
    print(f"\n  Writing {num_blocks} blocks ({num_blocks * BLOCK_SIZE:,} bytes)...\n")

    write_start = time.time()
    blocks_written = 0

    for blk in range(num_blocks):
        addr = start_addr + blk * BLOCK_SIZE
        offset = blk * BLOCK_SIZE
        block_data = bin_data[offset:offset + BLOCK_SIZE]

        # Pad if short
        if len(block_data) < BLOCK_SIZE:
            block_data = block_data + b'\xFF' * (BLOCK_SIZE - len(block_data))

        hex_data = block_data.hex().upper()
        wdata_line = f"WDATA:{addr:06X}:{hex_data}\r\n"
        ser.write(wdata_line.encode('ascii'))

        # Wait for ACK/NAK
        ack_deadline = time.time() + 30
        got_response = False
        while time.time() < ack_deadline:
            try:
                resp = ser.readline().decode('ascii', errors='replace').strip()
            except Exception:
                continue
            if not resp:
                continue

            if resp.startswith('WDATA:ACK:'):
                blocks_written += 1
                got_response = True
                break
            elif resp.startswith('WDATA:NAK:'):
                print(f"\n*** WRITE FAILED at block {blk} addr 0x{addr:06X}: {resp} ***")
                print("DO NOT POWER DOWN THE VEHICLE!")
                log_write_attempt(log_path, module, 'FAIL-WRITE', vin=vin, osid=osid,
                                  blocks=blocks_written, error=resp,
                                  elapsed=time.time() - start_time)
                return False
            elif resp.startswith('WRITE:PROGRESS') or resp.startswith('WRITE:TIMEOUT'):
                print(f"  < {resp}")
                if resp.startswith('WRITE:TIMEOUT'):
                    print("DO NOT POWER DOWN THE VEHICLE!")
                    return False
            elif resp.startswith('CRITICAL:'):
                print(f"  < {resp}")
                print("DO NOT POWER DOWN THE VEHICLE!")
                return False

        if not got_response:
            print(f"\n*** Timeout waiting for ACK at block {blk} ***")
            print("DO NOT POWER DOWN THE VEHICLE!")
            return False

        # Progress display
        elapsed = time.time() - write_start
        pct = (blk + 1) / num_blocks * 100
        if elapsed > 0 and blk > 0:
            eta = elapsed / (blk + 1) * (num_blocks - blk - 1)
            speed = (blk + 1) * BLOCK_SIZE / elapsed / 1024
            print(f"\r  Writing: {pct:5.1f}%  block {blk+1}/{num_blocks}  "
                  f"{speed:.1f} KB/s  ETA {eta:.0f}s  ", end='', flush=True)

    write_elapsed = time.time() - write_start
    print(f"\n\n  Write complete: {blocks_written} blocks in {write_elapsed:.1f}s "
          f"({blocks_written * BLOCK_SIZE / write_elapsed / 1024:.1f} KB/s)")

    # Read remaining firmware lines (WRITE:DONE, etc.)
    done_deadline = time.time() + 10
    while time.time() < done_deadline:
        try:
            line = ser.readline().decode('ascii', errors='replace').strip()
        except Exception:
            continue
        if not line:
            continue
        print(f"  < {line}")
        if line.startswith('WRITE:DONE') or line == 'OK':
            break

    if no_verify:
        print("\n  Verification skipped (--no-verify).")
        print("  Write appears complete but NOT verified.")
        # Still need to consume verify prompt and send OK
        time.sleep(1)
        send_cmd(ser, 'VERIFY:OK', echo=False)
        log_write_attempt(log_path, module, 'OK-NOVERIFY', vin=vin, osid=osid,
                          blocks=blocks_written, total_bytes=blocks_written * BLOCK_SIZE,
                          elapsed=time.time() - start_time)
        return True

    # ===== VERIFICATION PHASE =====
    print(f"\n  Verification readback starting...")
    print(f"  Capturing {num_blocks} blocks for comparison...\n")

    # Wait for FULLWRITE:VERIFY marker, then capture DATA: lines
    verify_blocks = {}
    verify_deadline = time.time() + timeout_total
    verify_started = False

    while time.time() < verify_deadline:
        try:
            line = ser.readline().decode('ascii', errors='replace').strip()
        except Exception:
            continue
        if not line:
            continue

        if line.startswith('DATA:'):
            parts = line.split(':', 2)
            if len(parts) == 3:
                try:
                    addr = int(parts[1], 16)
                    verify_blocks[addr] = bytes.fromhex(parts[2])
                    block_num = (addr - start_addr) // BLOCK_SIZE
                    if block_num % 64 == 0:
                        pct = (block_num + 1) / num_blocks * 100
                        print(f"\r  Verifying: {pct:5.1f}%  block {block_num+1}/{num_blocks}  ",
                              end='', flush=True)
                except ValueError:
                    print(f"\n  WARN: corrupt DATA at {parts[1]}, skipping")
            continue

        if 'VERIFY' in line or 'PROGRESS' in line or 'DONE' in line:
            print(f"  < {line}")
            if line.startswith('READ:DONE'):
                break
        elif line.startswith('ERR:'):
            print(f"  < {line}")
            break
        elif not verify_started and ('VERIFY' in line):
            verify_started = True
            print(f"  < {line}")

    print()  # newline after progress

    # Compare readback against original .bin
    mismatches = 0
    first_mismatch = None
    verified_blocks = 0

    for blk in range(num_blocks):
        addr = start_addr + blk * BLOCK_SIZE
        offset = blk * BLOCK_SIZE
        expected = bin_data[offset:offset + BLOCK_SIZE]

        if addr not in verify_blocks:
            if first_mismatch is None:
                first_mismatch = (blk, addr, "MISSING")
            mismatches += 1
            continue

        actual = verify_blocks[addr]
        if expected != actual:
            if first_mismatch is None:
                # Find first differing byte
                for i in range(min(len(expected), len(actual))):
                    if expected[i] != actual[i]:
                        first_mismatch = (blk, addr + i,
                                          f"expected={expected[i]:02X} got={actual[i]:02X}")
                        break
            mismatches += 1
        else:
            verified_blocks += 1

    total_elapsed = time.time() - start_time

    if mismatches == 0 and verified_blocks == num_blocks:
        # Compute CRC of readback
        readback_data = bytearray()
        for blk in range(num_blocks):
            addr = start_addr + blk * BLOCK_SIZE
            readback_data.extend(verify_blocks.get(addr, b'\xFF' * BLOCK_SIZE))
        readback_crc = zlib.crc32(bytes(readback_data)) & 0xFFFFFFFF
        original_crc = zlib.crc32(bin_data[:num_blocks * BLOCK_SIZE]) & 0xFFFFFFFF

        print(f"  VERIFICATION PASSED!")
        print(f"  Original CRC32: {original_crc:08X}")
        print(f"  Readback CRC32: {readback_crc:08X}")
        print(f"  Blocks verified: {verified_blocks}/{num_blocks}")
        print(f"  Total time: {total_elapsed:.1f}s")

        send_cmd(ser, 'VERIFY:OK', echo=False)
        log_write_attempt(log_path, module, 'OK', vin=vin, osid=osid,
                          filename=os.path.basename(args_bin_path),
                          blocks=blocks_written, total_bytes=blocks_written * BLOCK_SIZE,
                          elapsed=total_elapsed)

        # Read cleanup messages
        cleanup_deadline = time.time() + 10
        while time.time() < cleanup_deadline:
            try:
                line = ser.readline().decode('ascii', errors='replace').strip()
            except Exception:
                continue
            if not line:
                continue
            print(f"  < {line}")
            if 'DONE' in line or line == 'OK':
                break

        return True
    else:
        print(f"\n{'='*60}")
        print(f"  !! VERIFICATION FAILED !!")
        print(f"  Mismatched blocks: {mismatches}/{num_blocks}")
        if first_mismatch:
            blk_num, addr, detail = first_mismatch
            print(f"  First mismatch: block {blk_num} addr 0x{addr:06X} ({detail})")
        print(f"")
        print(f"  DO NOT POWER DOWN THE VEHICLE!")
        print(f"  The kernel is still running and can recover.")
        print(f"{'='*60}")

        send_cmd(ser, 'VERIFY:FAIL', echo=False)
        log_write_attempt(log_path, module, 'FAIL-VERIFY', vin=vin, osid=osid,
                          blocks=blocks_written, error=f"mismatches={mismatches}",
                          elapsed=total_elapsed)

        # Offer recovery options
        print(f"\n  Options:")
        print(f"    [R] Retry full write (erase + write + verify)")
        print(f"    [V] Re-verify only (readback compare)")
        print(f"    [A] Abort (kernel stays running, do NOT power down)")
        try:
            choice = input("\n  Choice [R/V/A]: ").strip().upper()
            if choice == 'R':
                print("\n  Retrying full write...\n")
                return do_write(ser, bin_data, module, is_cal, no_verify, timeout_total)
            elif choice == 'V':
                print("\n  Re-verifying (readback only)...")
                # TODO: implement standalone readback verify
                print("  Not yet implemented. Use manual READ command.")
        except (KeyboardInterrupt, EOFError):
            pass

        return False


# Global for logging
args_bin_path = ''


def main():
    global args_bin_path

    parser = argparse.ArgumentParser(description='Write .bin to ECU via J2534 Pass-Thru')
    parser.add_argument('binfile', help='Path to .bin file to write')
    parser.add_argument('port', nargs='?', default=None,
                        help='Serial port (e.g. COM38) — auto-detected if omitted')
    parser.add_argument('--baud', type=int, default=115200, help='Baud rate (default 115200)')
    parser.add_argument('--module', default='e38', help='Module type: e38, t87')
    parser.add_argument('--full', action='store_true', help='Full flash write (default: cal only)')
    parser.add_argument('--no-verify', action='store_true', help='Skip readback verification')
    parser.add_argument('--timeout', type=int, default=1200, help='Total timeout seconds')
    args = parser.parse_args()

    args_bin_path = args.binfile
    is_cal = not args.full

    # Pre-flight validation
    print(f"\nJ2534 Pass-Thru Flash Writer")
    print(f"{'='*40}")
    print(f"  Module:  {args.module.upper()}")
    print(f"  Mode:    {'Calibration only' if is_cal else 'FULL FLASH'}")

    bin_data = validate_bin_file(args.binfile, args.module, is_cal)
    if bin_data is None:
        sys.exit(1)

    # Confirmation prompt
    mode_str = 'CALIBRATION' if is_cal else 'FULL FLASH'
    print(f"\n  This will ERASE and REWRITE the ECU {mode_str}.")
    print(f"  Ensure stable power supply and DO NOT disconnect during write.")
    try:
        confirm = input(f"\n  Continue? [y/N]: ").strip().lower()
    except (KeyboardInterrupt, EOFError):
        print("\nAborted.")
        sys.exit(0)
    if confirm != 'y':
        print("Aborted.")
        sys.exit(0)

    # Connect
    port = args.port or find_feather_or_prompt()
    if not port:
        print("ERROR: No port specified and auto-detect failed.")
        sys.exit(1)

    print(f"\nOpening {port} at {args.baud} baud...")
    ser = serial.Serial(port, args.baud, timeout=1)
    time.sleep(2)

    # Drain boot messages
    while ser.in_waiting:
        line = ser.readline().decode('ascii', errors='replace').strip()
        if line:
            print(f"  < {line}")

    # Execute write
    success = do_write(ser, bin_data, args.module, is_cal, args.no_verify, args.timeout)

    ser.close()

    if success:
        print(f"\n  Write completed successfully!")
    else:
        print(f"\n  Write DID NOT complete successfully.")
        print(f"  If ECU is in programming mode, DO NOT power down.")

    sys.exit(0 if success else 1)


if __name__ == '__main__':
    try:
        main()
    except Exception as e:
        print(f"\n*** ERROR: {e} ***")
        import traceback
        traceback.print_exc()
        print("\n  If a write was in progress, DO NOT POWER DOWN THE VEHICLE!")
        if getattr(sys, 'frozen', False):
            input("\nPress Enter to close...")
        sys.exit(1)
