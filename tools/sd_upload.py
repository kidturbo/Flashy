#!/usr/bin/env python3
"""
Upload a file to the Feather M4 CAN SD card over serial.

Usage:
    python sd_upload.py <local_file> [sd_filename] [--port COM13]

The file is sent as hex-encoded chunks over serial using the SDWRITE command.
At 115200 baud with hex encoding, expect ~5 KB/s transfer rate.
A 4MB file takes about 14 minutes.
"""

import serial
import time
import sys
import os
import argparse

def find_feather():
    """Auto-detect Feather M4 CAN port."""
    try:
        from detect_port import find_feather as _find
        return _find()
    except:
        pass
    import serial.tools.list_ports
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x239A:  # Adafruit VID
            return p.device
    return None

def upload_file(port, local_path, sd_name, baud=115200):
    file_size = os.path.getsize(local_path)
    print(f"File: {local_path}")
    print(f"SD name: {sd_name}")
    print(f"Size: {file_size:,} bytes ({file_size/1024/1024:.2f} MB)")
    print(f"Port: {port}")
    print()

    ser = serial.Serial(port, baud, timeout=2)
    time.sleep(2)
    # Drain boot messages
    while ser.in_waiting:
        ser.readline()

    # Send SDWRITE command
    cmd = f"SDWRITE {sd_name} {file_size}\r\n"
    ser.write(cmd.encode())
    time.sleep(0.5)

    # Wait for READY
    ready = False
    deadline = time.time() + 5
    while time.time() < deadline:
        line = ser.readline().decode('ascii', errors='replace').strip()
        if line:
            print(f"  < {line}")
            if 'SDWRITE:READY' in line:
                ready = True
                break
            if line.startswith('ERR:'):
                print(f"ERROR: {line}")
                ser.close()
                return False

    if not ready:
        print("ERROR: No READY response from Feather")
        ser.close()
        return False

    # Send file in chunks (256 bytes per line = 512 hex chars)
    CHUNK_SIZE = 256
    sent = 0
    start_time = time.time()

    with open(local_path, 'rb') as f:
        while sent < file_size:
            chunk = f.read(CHUNK_SIZE)
            if not chunk:
                break

            # Hex-encode and send
            hex_str = chunk.hex().upper() + "\r\n"
            ser.write(hex_str.encode())
            sent += len(chunk)

            # Wait for ACK
            ack_deadline = time.time() + 10
            while time.time() < ack_deadline:
                line = ser.readline().decode('ascii', errors='replace').strip()
                if 'SDWRITE:ACK' in line:
                    break
                if line.startswith('ERR:'):
                    print(f"\nERROR: {line}")
                    ser.write(b"SDABORT\r\n")
                    ser.close()
                    return False

            # Progress
            elapsed = time.time() - start_time
            rate = sent / elapsed / 1024 if elapsed > 0 else 0
            pct = sent * 100 / file_size
            eta = (file_size - sent) / (sent / elapsed) if sent > 0 else 0
            print(f"\r  {sent:,}/{file_size:,} bytes ({pct:.1f}%) {rate:.1f} KB/s ETA {eta:.0f}s   ", end='', flush=True)

    print()

    # Send SDDONE
    ser.write(b"SDDONE\r\n")
    time.sleep(1)

    done = False
    deadline = time.time() + 5
    while time.time() < deadline:
        line = ser.readline().decode('ascii', errors='replace').strip()
        if line:
            print(f"  < {line}")
            if 'SDWRITE:DONE' in line:
                done = True
                break

    elapsed = time.time() - start_time
    ser.close()

    if done:
        print(f"\nUpload complete: {file_size:,} bytes in {elapsed:.1f}s ({file_size/elapsed/1024:.1f} KB/s)")
        return True
    else:
        print("\nWARNING: No DONE confirmation")
        return False

def main():
    parser = argparse.ArgumentParser(description='Upload file to Feather SD card')
    parser.add_argument('file', help='Local file to upload')
    parser.add_argument('sdname', nargs='?', help='Filename on SD card (default: same as local)')
    parser.add_argument('--port', '-p', help='Serial port (default: auto-detect)')
    args = parser.parse_args()

    if not os.path.exists(args.file):
        print(f"ERROR: File not found: {args.file}")
        sys.exit(1)

    sd_name = args.sdname or os.path.basename(args.file)
    port = args.port or find_feather()

    if not port:
        print("ERROR: Feather not found. Specify --port")
        sys.exit(1)

    success = upload_file(port, args.file, sd_name)
    sys.exit(0 if success else 1)

if __name__ == '__main__':
    main()
