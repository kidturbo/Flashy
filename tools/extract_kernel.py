#!/usr/bin/env python3
"""Extract the T87 write kernel from a SavvyCAN CSV capture.

Parses the ISO-TP $36 TransferData frames on CAN ID 0x7E2 that upload
the kernel after $34 RequestDownload.
"""
import sys
import csv
import struct

if len(sys.argv) < 2:
    sys.exit("usage: extract_kernel.py <savvycan_capture.csv>")
csv_path = sys.argv[1]

frames = []
with open(csv_path, newline='') as f:
    reader = csv.reader(f)
    header = next(reader)
    for row in reader:
        if len(row) < 10:
            continue
        can_id = int(row[1], 16)
        dlc = int(row[5])
        data = [int(row[6+i], 16) for i in range(min(dlc, 8))]
        frames.append((can_id, data))

# Find $34 RequestDownload on 0x7E2
print("=== Searching for $34 RequestDownload ===")
for i, (cid, d) in enumerate(frames):
    if cid == 0x7E2 and len(d) >= 2 and d[1] == 0x34:
        print(f"  Frame {i}: ID=0x{cid:03X} data={' '.join(f'{b:02X}' for b in d)}")

# Find $74 positive response on 0x7EA
for i, (cid, d) in enumerate(frames):
    if cid == 0x7EA and len(d) >= 2 and (d[0] == 0x74 or (len(d) > 1 and d[1] == 0x74)):
        print(f"  Response frame {i}: ID=0x{cid:03X} data={' '.join(f'{b:02X}' for b in d)}")

# Find ISO-TP First Frame for $36 on 0x7E2 (kernel upload)
# First Frame format: [1X XX] where 1 = FF indicator, XXX = length
print("\n=== Searching for $36 TransferData ISO-TP First Frame ===")
kernel_start_idx = None
isotp_length = 0
for i, (cid, d) in enumerate(frames):
    if cid == 0x7E2 and len(d) >= 8 and (d[0] & 0xF0) == 0x10:
        # ISO-TP First Frame
        length = ((d[0] & 0x0F) << 8) | d[1]
        sid = d[2]
        if sid == 0x36:
            print(f"  Frame {i}: FF length={length} (0x{length:X})")
            print(f"  Data: {' '.join(f'{b:02X}' for b in d)}")
            print(f"  SID=0x{d[2]:02X} sub=0x{d[3]:02X} addr={d[4]:02X}{d[5]:02X}{d[6]:02X}{d[7]:02X}")
            kernel_start_idx = i
            isotp_length = length
            break

if kernel_start_idx is None:
    print("ERROR: No $36 ISO-TP First Frame found")
    sys.exit(1)

# Extract kernel bytes
# First Frame: bytes 2-7 are payload (after the 2-byte FF header)
ff_data = frames[kernel_start_idx][1]
payload = list(ff_data[2:])  # skip FF header bytes, get 6 payload bytes

# The payload starts with: 36 <sub> <addr:4> <kernel_data...>
# So: 36 00 40 02 B0 00 <kernel bytes start here>
# First 6 bytes of payload from FF: [36, 00, 40, 02, B0, 00] = header, no kernel data in FF
# Wait - FF has 6 payload bytes: d[2..7] = 36 00 40 02 B0 00
# So no kernel data in the first frame itself, it's all header

# Total payload = isotp_length bytes
# Payload = [36] [00] [40 02 B0 00] [kernel_data...]
# kernel_data_size = isotp_length - 6 (SID + sub + 4-byte addr)
kernel_header_size = 6  # 36 00 40 02 B0 00
kernel_data_size = isotp_length - kernel_header_size
print(f"\n  ISO-TP total payload: {isotp_length} bytes")
print(f"  Kernel header: {kernel_header_size} bytes (36 00 + 4-byte addr)")
print(f"  Kernel data: {kernel_data_size} bytes")

# Collect consecutive frames
all_payload = payload  # 6 bytes from FF (this is the $36 header)
cf_count = 0
expected_seq = 1
for i in range(kernel_start_idx + 1, len(frames)):
    cid, d = frames[i]
    if cid == 0x7EA:
        # Skip flow control frames from TCM
        continue
    if cid != 0x7E2:
        continue
    if len(d) < 2:
        continue
    if (d[0] & 0xF0) != 0x20:
        # Not a consecutive frame - might be done
        print(f"  Non-CF at frame {i}: {' '.join(f'{b:02X}' for b in d)}")
        break
    seq = d[0] & 0x0F
    if seq != (expected_seq & 0x0F):
        print(f"  WARNING: expected seq {expected_seq & 0x0F}, got {seq} at frame {i}")
    cf_data = list(d[1:])  # 7 bytes per CF
    all_payload.extend(cf_data)
    cf_count += 1
    expected_seq += 1

# Trim to exact ISO-TP length
all_payload = all_payload[:isotp_length]
print(f"\n  Consecutive frames: {cf_count}")
print(f"  Total payload collected: {len(all_payload)} bytes")

# Extract kernel bytes (skip the 6-byte $36 header)
kernel_bytes = all_payload[kernel_header_size:]
print(f"  Kernel bytes: {len(kernel_bytes)}")

# Show first 64 bytes
print(f"\n=== Kernel Header (first 64 bytes) ===")
for row_off in range(0, min(64, len(kernel_bytes)), 16):
    hex_str = ' '.join(f'{kernel_bytes[row_off+j]:02X}' for j in range(min(16, len(kernel_bytes) - row_off)))
    print(f"  {row_off:04X}: {hex_str}")

# Show last 64 bytes
print(f"\n=== Kernel Tail (last 64 bytes) ===")
start = max(0, len(kernel_bytes) - 64)
for row_off in range(start, len(kernel_bytes), 16):
    hex_str = ' '.join(f'{kernel_bytes[row_off+j]:02X}' for j in range(min(16, len(kernel_bytes) - row_off)))
    print(f"  {row_off:04X}: {hex_str}")

# Look for any embedded copyright / version string
kernel_str = bytes(kernel_bytes)
for needle in [b"(c)20", b"T87_", b"v1.0", b"v0.", b"FLSHY"]:
    idx = kernel_str.find(needle)
    if idx >= 0:
        print(f"  Found '{needle.decode()}' at offset 0x{idx:X}: {kernel_str[idx:idx+40]}")
        break

# Find execute command ($36 80)
print(f"\n=== Searching for $36 80 Execute Command ===")
for i in range(kernel_start_idx + cf_count + 1, min(kernel_start_idx + cf_count + 50, len(frames))):
    cid, d = frames[i]
    if cid == 0x7E2 and len(d) >= 2:
        print(f"  Frame {i}: ID=0x{cid:03X} data={' '.join(f'{b:02X}' for b in d)}")
        if d[1] == 0x36 or (len(d) > 2 and d[2] == 0x36):
            break

# Write raw binary
bin_path = csv_path.replace('.csv', '_kernel.bin')
with open(bin_path, 'wb') as f:
    f.write(bytes(kernel_bytes))
print(f"\n  Wrote {len(kernel_bytes)} bytes to: {bin_path}")

# Write C header
h_path = csv_path.replace('.csv', '_kernel.h')
with open(h_path, 'w') as f:
    f.write(f"/* T87 Write Kernel — extracted from {csv_path.split(chr(92))[-1]} */\n")
    f.write(f"#define T87_WRITE_KERNEL_SIZE  {len(kernel_bytes)}\n")
    f.write(f"#define T87_WRITE_KERNEL_LOAD_ADDR  0x4002B000UL\n\n")
    f.write(f"static const uint8_t T87_WRITE_KERNEL[] = {{\n")
    for row_off in range(0, len(kernel_bytes), 16):
        chunk = kernel_bytes[row_off:row_off+16]
        hex_str = ', '.join(f'0x{b:02X}' for b in chunk)
        if row_off + 16 < len(kernel_bytes):
            hex_str += ','
        f.write(f"    {hex_str}\n")
    f.write(f"}};\n")
print(f"  Wrote C header to: {h_path}")

# Also find the write data blocks (after erase)
print(f"\n=== Write Data Blocks (first 5 after erase) ===")
write_count = 0
for i, (cid, d) in enumerate(frames):
    if cid == 0x7E2 and len(d) >= 8 and (d[0] & 0xF0) == 0x10:
        length = ((d[0] & 0x0F) << 8) | d[1]
        if d[2] == 0x36 and d[3] == 0x00 and length > 100:
            addr = (d[4] << 24) | (d[5] << 16) | (d[6] << 8) | d[7]
            # Skip the kernel upload (addr 0x4002B000)
            if addr == 0x4002B000:
                continue
            write_count += 1
            print(f"  Block {write_count}: addr=0x{addr:08X} length={length}")
            if write_count >= 5:
                break

# Count total write blocks
total_writes = 0
write_addrs = []
for i, (cid, d) in enumerate(frames):
    if cid == 0x7E2 and len(d) >= 8 and (d[0] & 0xF0) == 0x10:
        length = ((d[0] & 0x0F) << 8) | d[1]
        if d[2] == 0x36 and d[3] == 0x00 and length > 100:
            addr = (d[4] << 24) | (d[5] << 16) | (d[6] << 8) | d[7]
            if addr != 0x4002B000:
                total_writes += 1
                write_addrs.append(addr)

if write_addrs:
    print(f"\n  Total write blocks: {total_writes}")
    print(f"  Address range: 0x{min(write_addrs):08X} - 0x{max(write_addrs):08X}")
    print(f"  Total data: {total_writes * 2048} bytes ({total_writes * 2048 / 1024:.0f} KB)")
