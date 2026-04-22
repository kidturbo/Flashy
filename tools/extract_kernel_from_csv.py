#!/usr/bin/env python3
"""Extract kernel binary from SavvyCAN CSV capture of ISO-TP upload."""
import sys, csv

def extract_kernel(csv_path, tx_id=0x7E0, rx_id=0x7E8):
    """Extract kernel from $36 TransferData ISO-TP multi-frame on tx_id."""

    frames = []
    with open(csv_path, 'r') as f:
        reader = csv.reader(f)
        header = next(reader)
        for row in reader:
            if len(row) < 14:
                continue
            can_id = int(row[1], 16)
            if can_id != tx_id:
                continue
            dlc = int(row[5])
            data = []
            for i in range(6, 6 + min(dlc, 8)):
                try:
                    data.append(int(row[i], 16))
                except (ValueError, IndexError):
                    data.append(0)
            frames.append(data)

    print(f"Total TX frames (0x{tx_id:03X}): {len(frames)}")

    # Find the kernel upload First Frame: starts with 1x xx 36 00
    # Look for FF with $36 and large payload (kernel upload, not small messages)
    kernel_start = None
    for i, d in enumerate(frames):
        if len(d) >= 4 and (d[0] >> 4) == 1 and d[2] == 0x36:
            ff_len = ((d[0] & 0x0F) << 8) | d[1]
            if ff_len > 500:  # kernel should be large
                kernel_start = i
                print(f"Found kernel FF at frame {i}: len={ff_len}, data={' '.join(f'{b:02X}' for b in d)}")
                break

    if kernel_start is None:
        print("ERROR: No kernel upload First Frame found")
        return None

    # Parse First Frame
    ff = frames[kernel_start]
    isotp_len = ((ff[0] & 0x0F) << 8) | ff[1]
    print(f"ISO-TP payload length: {isotp_len}")

    # Collect UDS payload from FF (bytes 2-7) + CFs
    payload = bytes(ff[2:8])  # 6 bytes from FF

    cf_index = kernel_start + 1
    expected_seq = 1
    while len(payload) < isotp_len and cf_index < len(frames):
        cf = frames[cf_index]
        frame_type = (cf[0] >> 4) & 0x0F

        if frame_type == 2:  # Consecutive Frame
            seq = cf[0] & 0x0F
            if seq != (expected_seq & 0x0F):
                print(f"WARN: expected seq {expected_seq & 0x0F}, got {seq} at frame {cf_index}")
            payload += bytes(cf[1:8])  # 7 bytes from CF
            expected_seq += 1
            cf_index += 1
        elif frame_type == 3:  # Flow Control (from our side — skip)
            cf_index += 1
        else:
            # Hit a non-CF frame (maybe next message)
            cf_index += 1
            # Check if it's a Single Frame or new FF — stop
            if frame_type == 0 or frame_type == 1:
                break

    # Trim to exact length
    payload = payload[:isotp_len]
    print(f"UDS payload: {len(payload)} bytes")
    print(f"First 10 bytes: {' '.join(f'{b:02X}' for b in payload[:10])}")

    # UDS: $36 [block_seq] [addr:4] [kernel_data...]
    sid = payload[0]
    block_seq = payload[1]
    addr = (payload[2] << 24) | (payload[3] << 16) | (payload[4] << 8) | payload[5]
    kernel_data = payload[6:]

    print(f"SID: 0x{sid:02X}, block_seq: 0x{block_seq:02X}")
    print(f"Load address: 0x{addr:08X}")
    print(f"Kernel binary: {len(kernel_data)} bytes")

    # Check for version string
    try:
        text = kernel_data.decode('ascii', errors='ignore')
        import re
        versions = re.findall(r'[\w\(\)]+\s+E\w+_v[\d.]+\w*', text)
        if versions:
            print(f"Version strings found: {versions}")
        efi_match = re.search(r'\(c\)\d{4}\s+EFILIVE\s+\S+', text)
        if efi_match:
            print(f"Copyright: {efi_match.group()}")
    except:
        pass

    # Show last 32 bytes (often has version string)
    print(f"Last 32 bytes: {' '.join(f'{b:02X}' for b in kernel_data[-32:])}")
    try:
        print(f"  ASCII: {kernel_data[-32:].decode('ascii', errors='replace')}")
    except:
        pass

    return kernel_data, addr

if __name__ == '__main__':
    import os

    if len(sys.argv) < 2:
        print("Usage: extract_kernel_from_csv.py <capture.csv> [output.bin] [tx_id] [rx_id]")
        print("  tx_id / rx_id default to 0x7E0 / 0x7E8 (hex, no prefix)")
        sys.exit(1)

    csv_path = sys.argv[1]

    # Default output: same dir as input, <basename>_kernel.bin
    if len(sys.argv) > 2:
        out_path = sys.argv[2]
    else:
        base = os.path.splitext(os.path.basename(csv_path))[0]
        out_path = os.path.join(os.path.dirname(csv_path) or '.', f'{base}_kernel.bin')

    tx_id = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x7E0
    rx_id = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x7E8

    result = extract_kernel(csv_path, tx_id=tx_id, rx_id=rx_id)
    if result:
        kernel_data, addr = result
        with open(out_path, 'wb') as f:
            f.write(kernel_data)
        print(f"\nSaved: {out_path} ({len(kernel_data)} bytes)")
        print(f"Load address: 0x{addr:08X}")
