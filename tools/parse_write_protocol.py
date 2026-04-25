#!/usr/bin/env python3
"""Parse the T87 write protocol sequence from a SavvyCAN CSV capture.
Shows the high-level phases: diagnostics, security, kernel, erase, write, finalize.
"""
import sys
import csv

if len(sys.argv) < 2:
    sys.exit("usage: parse_write_protocol.py <savvycan_capture.csv>")
csv_path = sys.argv[1]

frames = []
with open(csv_path, newline='') as f:
    reader = csv.reader(f)
    header = next(reader)
    for row in reader:
        if len(row) < 10:
            continue
        ts = int(row[0]) if row[0].isdigit() else 0
        can_id = int(row[1], 16)
        dlc = int(row[5])
        data = [int(row[6+i], 16) for i in range(min(dlc, 8))]
        frames.append((ts, can_id, data))

print(f"Total frames: {len(frames)}")
print(f"File: {csv_path.split(chr(92))[-1]}")
print()

# Track protocol events
first_ts = frames[0][0] if frames else 0

def ts_sec(ts):
    return (ts - first_ts) / 1_000_000

# CAN ID distribution
id_counts = {}
for ts, cid, d in frames:
    id_counts[cid] = id_counts.get(cid, 0) + 1

print("=== CAN ID Distribution ===")
for cid in sorted(id_counts.keys()):
    print(f"  0x{cid:03X}: {id_counts[cid]:>6} frames")
print()

# Parse diagnostic frames on 0x7E2/0x7EA/0x101
print("=== Protocol Sequence ===")

# Track state
in_kernel_upload = False
kernel_cf_count = 0
in_write_block = False
write_block_count = 0
write_cf_count = 0
first_write_addr = None
last_write_addr = None
write_addrs = []

for i, (ts, cid, d) in enumerate(frames):
    t = ts_sec(ts)

    # Broadcast on 0x101
    if cid == 0x101 and len(d) >= 3:
        svc = d[2] if len(d) > 2 else 0
        sub = d[3] if len(d) > 3 else 0
        if svc == 0x3E:
            continue  # skip TesterPresent
        names = {
            0x20: "returnToNormalMode",
            0x10: f"DiagSessionControl sub={sub:02X}",
            0x28: "disableNormalComm",
            0xA2: "reportProgrammingState",
            0xA5: f"programmingMode sub={sub:02X}",
            0x04: "clearDiagInfo",
            0x14: "clearDTCs",
        }
        name = names.get(svc, f"SID={svc:02X}")
        print(f"  [{t:7.2f}s] 0x101 BROADCAST: {name}  [{' '.join(f'{b:02X}' for b in d)}]")
        continue

    # Tester -> TCM on 0x7E2
    if cid == 0x7E2:
        # Single frame (SF)
        if (d[0] & 0xF0) == 0x00:
            pci_len = d[0] & 0x0F
            sid = d[1] if pci_len >= 1 else 0

            if sid == 0x34:
                print(f"  [{t:7.2f}s] 0x7E2 TX: $34 RequestDownload  [{' '.join(f'{b:02X}' for b in d)}]")
            elif sid == 0x27:
                sub = d[2] if len(d) > 2 else 0
                if sub == 0x01:
                    print(f"  [{t:7.2f}s] 0x7E2 TX: $27 SecurityAccess SEED REQUEST")
                elif sub == 0x02:
                    key = ' '.join(f'{d[j]:02X}' for j in range(3, min(3+pci_len-2, len(d))))
                    print(f"  [{t:7.2f}s] 0x7E2 TX: $27 SecurityAccess KEY={key}")
            elif sid == 0x36:
                sub = d[2] if len(d) > 2 else 0
                if sub == 0x80:
                    addr = ''.join(f'{d[j]:02X}' for j in range(3, min(7, len(d))))
                    print(f"  [{t:7.2f}s] 0x7E2 TX: $36 80 EXECUTE at 0x{addr}")
                elif sub == 0xEE:
                    print(f"  [{t:7.2f}s] 0x7E2 TX: $36 EE ERASE/STATUS QUERY")
                elif sub == 0xFF:
                    print(f"  [{t:7.2f}s] 0x7E2 TX: $36 FF FINALIZE")
                else:
                    print(f"  [{t:7.2f}s] 0x7E2 TX: $36 {sub:02X}  [{' '.join(f'{b:02X}' for b in d)}]")
            elif sid == 0x1A:
                sub = d[2] if len(d) > 2 else 0
                names_1a = {0xB0: "moduleType", 0x55: "FNA", 0x90: "VIN", 0xB4: "serial",
                           0xC0: "bootSWver", 0xC1: "appSWver"}
                name = names_1a.get(sub, f"sub={sub:02X}")
                print(f"  [{t:7.2f}s] 0x7E2 TX: $1A ReadDID {name}")
            elif sid == 0x3E:
                continue  # TesterPresent
            else:
                print(f"  [{t:7.2f}s] 0x7E2 TX: SID=${sid:02X}  [{' '.join(f'{b:02X}' for b in d)}]")

        # First Frame (FF) - kernel upload or write block
        elif (d[0] & 0xF0) == 0x10:
            length = ((d[0] & 0x0F) << 8) | d[1]
            sid = d[2]
            if sid == 0x36 and d[3] == 0x00:
                addr = (d[4] << 24) | (d[5] << 16) | (d[6] << 8) | d[7]
                if addr == 0x4002B000:
                    print(f"  [{t:7.2f}s] 0x7E2 TX: $36 KERNEL UPLOAD ({length-6} bytes to 0x{addr:08X})")
                    in_kernel_upload = True
                    kernel_cf_count = 0
                else:
                    if write_block_count == 0:
                        print(f"  [{t:7.2f}s] 0x7E2 TX: $36 WRITE BLOCK 1 addr=0x{addr:08X} ({length-6} bytes)")
                        first_write_addr = addr
                    write_block_count += 1
                    last_write_addr = addr
                    write_addrs.append(addr)
                    in_write_block = True
                    write_cf_count = 0

        # Consecutive Frame (CF) - part of kernel or write
        elif (d[0] & 0xF0) == 0x20:
            if in_kernel_upload:
                kernel_cf_count += 1
            elif in_write_block:
                write_cf_count += 1
            continue

    # TCM -> Tester on 0x7EA
    if cid == 0x7EA:
        # Single frame responses
        if (d[0] & 0xF0) == 0x00:
            pci_len = d[0] & 0x0F
            sid = d[1] if pci_len >= 1 else 0

            if sid == 0x74:
                print(f"  [{t:7.2f}s] 0x7EA RX: $74 RequestDownload ACCEPTED")
            elif sid == 0x76:
                sub = d[2] if len(d) > 2 else 0
                code = d[3] if len(d) > 3 else 0
                code_names = {0x73: "WRITE_OK", 0xEE: "ERASE_DONE", 0x86: "FINAL_OK"}
                name = code_names.get(code, f"0x{code:02X}")
                if code == 0xEE:
                    print(f"  [{t:7.2f}s] 0x7EA RX: $76 ERASE COMPLETE (status={sub:02X} code={name})")
                elif code == 0x86:
                    print(f"  [{t:7.2f}s] 0x7EA RX: $76 FINALIZE OK (status={sub:02X} code={name})")
                elif code == 0x73 and write_block_count <= 1:
                    print(f"  [{t:7.2f}s] 0x7EA RX: $76 WRITE_OK block 1")
                # Don't spam every write OK
            elif sid == 0x67:
                sub = d[2] if len(d) > 2 else 0
                if sub == 0x01:
                    seed = ' '.join(f'{d[j]:02X}' for j in range(3, min(3+pci_len-2, len(d))))
                    print(f"  [{t:7.2f}s] 0x7EA RX: $67 SEED={seed}")
                elif sub == 0x02:
                    print(f"  [{t:7.2f}s] 0x7EA RX: $67 KEY ACCEPTED")
            elif sid == 0xE5:
                print(f"  [{t:7.2f}s] 0x7EA RX: $E5 programmingMode OK")
            elif sid == 0x7F:
                nrc_sid = d[2] if len(d) > 2 else 0
                nrc = d[3] if len(d) > 3 else 0
                print(f"  [{t:7.2f}s] 0x7EA RX: $7F NEGATIVE SID=${nrc_sid:02X} NRC=0x{nrc:02X}")
            elif sid == 0x50:
                print(f"  [{t:7.2f}s] 0x7EA RX: $50 DiagSession OK")

        # Flow control
        elif (d[0] & 0xF0) == 0x30:
            bs = d[1]
            stmin = d[2]
            if in_kernel_upload:
                print(f"  [{t:7.2f}s] 0x7EA RX: FC BS={bs} STmin=0x{stmin:02X} (kernel upload)")
                in_kernel_upload = False
            elif in_write_block and write_block_count <= 1:
                print(f"  [{t:7.2f}s] 0x7EA RX: FC BS={bs} STmin=0x{stmin:02X} (write data)")
                in_write_block = False

# Summary
if write_block_count > 0:
    print(f"\n  ... ({write_block_count-1} more write blocks) ...")
    print(f"\n=== Write Summary ===")
    print(f"  Total write blocks: {write_block_count}")
    print(f"  First addr: 0x{first_write_addr:08X}")
    print(f"  Last addr:  0x{last_write_addr:08X}")
    print(f"  Total data: {write_block_count * 2048} bytes ({write_block_count * 2048 / 1024:.0f} KB)")

    # Detect gaps
    if len(write_addrs) > 1:
        write_addrs.sort()
        gaps = []
        for j in range(1, len(write_addrs)):
            if write_addrs[j] - write_addrs[j-1] > 0x800:
                gaps.append((write_addrs[j-1] + 0x800, write_addrs[j]))
        if gaps:
            print(f"\n  Address gaps (skipped regions):")
            for gap_start, gap_end in gaps:
                print(f"    0x{gap_start:06X} - 0x{gap_end:06X} ({(gap_end - gap_start)//1024} KB)")
