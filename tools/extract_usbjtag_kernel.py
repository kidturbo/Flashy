#!/usr/bin/env python3
"""Extract the USBJTAG E38 kernel binary from a CAN capture CSV.

The kernel is uploaded via ISO-TP: $34 RequestDownload, then $36 $80 TransferData
with consecutive frames on CAN ID 0x7E0.

Usage: python extract_usbjtag_kernel.py <csv_file> [output.bin]
"""

import sys
import csv
import os

def extract_kernel(csv_path, out_path=None):
    if out_path is None:
        out_path = os.path.join(os.path.dirname(csv_path), "usbjtag_e38_kernel.bin")

    rows = []
    with open(csv_path, 'r') as f:
        reader = csv.reader(f)
        header = next(reader)  # skip header
        for row in reader:
            rows.append(row)

    # Find the First Frame on 0x7E0 that starts the $36 $80 transfer
    ff_idx = None
    for i, row in enumerate(rows):
        can_id = row[1].strip()
        if can_id != '000007E0':
            continue
        d1 = int(row[6].strip(), 16)
        d2 = int(row[7].strip(), 16)
        d3 = int(row[8].strip(), 16)
        d4 = int(row[9].strip(), 16)
        # ISO-TP First Frame: high nibble of D1 = 0x1, and UDS service = 0x36
        if (d1 >> 4) == 1 and d3 == 0x36 and d4 == 0x80:
            ff_idx = i
            # ISO-TP length = (D1 & 0x0F) << 8 | D2
            isotp_len = ((d1 & 0x0F) << 8) | d2
            print(f"Found First Frame at CSV line {i+2}")
            print(f"  ISO-TP payload length: {isotp_len} (0x{isotp_len:04X})")
            print(f"  UDS: $36 $80 (TransferData downloadAndExecute)")
            # First 6 data bytes of FF are: D3-D8 = 36 80 addr[4]
            break

    if ff_idx is None:
        print("ERROR: Could not find $36 $80 First Frame on 0x7E0")
        sys.exit(1)

    # Extract the UDS payload from the ISO-TP transfer
    # FF provides 6 bytes of payload (D3-D8)
    payload = []
    ff_row = rows[ff_idx]
    for j in range(8, 14):  # D3 through D8
        payload.append(int(ff_row[j].strip(), 16))

    remaining = isotp_len - 6  # bytes still needed from CFs
    expected_seq = 1  # next CF sequence number (wraps 0-15 -> 0x21-0x2F, 0x20)

    # Walk through subsequent rows looking for Consecutive Frames on 0x7E0
    i = ff_idx + 1
    while remaining > 0 and i < len(rows):
        row = rows[i]
        can_id = row[1].strip()
        i += 1
        if can_id != '000007E0':
            continue

        d1 = int(row[6].strip(), 16)
        pci_type = d1 >> 4

        if pci_type != 2:  # Not a Consecutive Frame
            continue

        seq = d1 & 0x0F
        expected = expected_seq & 0x0F
        if seq != expected:
            print(f"WARNING: Expected CF seq {expected}, got {seq} at CSV line {i+1}")

        # CF provides up to 7 bytes (D2-D8)
        n_bytes = min(7, remaining)
        for j in range(7, 7 + n_bytes):  # D2 through D8
            payload.append(int(row[j].strip(), 16))
        remaining -= n_bytes
        expected_seq += 1

    print(f"  Total UDS payload extracted: {len(payload)} bytes")
    print(f"  Expected: {isotp_len} bytes")

    if len(payload) != isotp_len:
        print(f"WARNING: Size mismatch! Got {len(payload)}, expected {isotp_len}")

    # The UDS payload is: 36 80 [addr:4] [kernel_data...]
    # Strip the $36 $80 service bytes and 4-byte address
    svc = payload[0]
    block_seq = payload[1]
    load_addr = (payload[2] << 24) | (payload[3] << 16) | (payload[4] << 8) | payload[5]
    kernel_data = bytes(payload[6:])

    print(f"\n  Service: ${svc:02X}")
    print(f"  Block seq: ${block_seq:02X} (downloadAndExecute)")
    print(f"  Load address: 0x{load_addr:08X}")
    print(f"  Kernel size: {len(kernel_data)} bytes (0x{len(kernel_data):04X})")

    # Save kernel binary
    with open(out_path, 'wb') as f:
        f.write(kernel_data)
    print(f"\n  Saved kernel to: {out_path}")

    # Also generate C header array
    h_path = out_path.replace('.bin', '.h')
    with open(h_path, 'w') as f:
        f.write(f"/* USBJTAG E38 kernel — extracted from CAN capture */\n")
        f.write(f"/* Load address: 0x{load_addr:08X} */\n")
        f.write(f"/* Size: {len(kernel_data)} bytes */\n\n")
        f.write(f"#define E38_USBJTAG_KERNEL_SIZE     {len(kernel_data)}\n")
        f.write(f"#define E38_USBJTAG_LOAD_ADDR       0x{load_addr:08X}UL\n\n")
        f.write(f"static const uint8_t E38_USBJTAG_KERNEL[] = {{\n")
        for j in range(0, len(kernel_data), 16):
            chunk = kernel_data[j:j+16]
            hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
            f.write(f"    {hex_str},\n")
        f.write(f"}};\n")
    print(f"  Saved C header to: {h_path}")

    # Print first 64 bytes for verification
    print(f"\n  First 64 bytes of kernel:")
    for j in range(0, min(64, len(kernel_data)), 16):
        chunk = kernel_data[j:j+16]
        hex_str = " ".join(f"{b:02X}" for b in chunk)
        print(f"    {j:04X}: {hex_str}")

    return kernel_data

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <csv_file> [output.bin]")
        sys.exit(1)
    csv_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else None
    extract_kernel(csv_path, out_path)
