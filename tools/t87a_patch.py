#!/usr/bin/env python3
"""
T87A 5-Patch Dual Unlock Tool with Checksum Recalculation

Applies the 5-patch recipe for USBJTAG kernel execute + HPT unlock,
then recalculates the Boot Block CRC16 and Wordsum checksums.

VERIFIED WORKING: 2026-03-21
  - Kernel: USBJTAG heartbeat after 1 frame
  - HPT: Reports "All Unlocked"
  - Checksums: Match USBJTAG tool output exactly
  - Tested on OS 24288836 (primary bench-verified)
  - 2026-04-22: per-OS offset table added (OS_PATCHES). 24286985 v2 bin
    generated with correct offsets — untested on bench as of this writing.

Usage:
    python t87a_patch.py <input.bin> [output.bin] [--no-bam]
    python t87a_patch.py --verify <file.bin>
    python t87a_patch.py --batch <directory>

Flags:
    --no-bam   Skip emission of the legacy -BAM.bin sidecar (kept for historical
               BAM-only workflows; no longer needed now that BAM writes the full
               image and we handle the 0x020000 offset in firmware).

Per-OS offsets:
  - Default (24288836 recipe) is used unless the OS PN at 0x014638 matches a
    key in OS_PATCHES. Adding a new OS means adding its entry to OS_PATCHES.
    See the docstring above OS_PATCHES for fingerprint-based offset discovery.
"""

import struct
import sys
import os
import shutil
import zlib

# ── Per-OS 5-Patch Recipe Table ─────────────────────────────────
#
# How to add a new OS variant:
#   1. Find the STOCK (pre-5-patch) bin for the OS.
#   2. Locate P1 with this fingerprint fragment:
#        3B E0 00 01 ... 63 XX YY ZZ ... B3 XD 00 00 2C 1F 00 00
#      The `40 82 NN NN` bne IMMEDIATELY after the 2C1F0000 cmpwi is the P1 site.
#   3. P2 is at P1 + 0x64 (stable intra-function offset on studied OSes).
#   4. Locate P3 with fragment:  38 7E 01 1A 7F 44 D3 78 7F A5 EB 78
#      P3 = fragment offset + 0x18.
#   5. Locate P3a/P3b using this longer fragment:
#        7D 5C 50 AE 2C 0A 00 80 40 82 00 0C <bl-4B> <b-48>
#      P3a = offset of the bl (first 4B FF FD XX)
#      P3b = P3a + 4 (the b that follows)
#   6. Sanity-check each candidate: expected_old bytes must appear at the offset
#      in the stock bin BEFORE adding the entry here.
#
# DEFAULT table is the 24288836 recipe — matches OSes whose compiler laid out
# the HPT check region byte-for-byte with 836 (includes 24293216).
DEFAULT_PATCHES = [
    # (offset, old_bytes, new_bytes, description)
    (0x02B39C, bytes([0x4B, 0xFF, 0xFD, 0x11]), bytes([0x39, 0x60, 0x00, 0x01]),
     "USBJTAG P3a: li r11,1 (set valid flag)"),
    (0x02B3A0, bytes([0x48, 0x00, 0x01, 0xC4]), bytes([0x99, 0x6C, 0xDB, 0x23]),
     "USBJTAG P3b: stb r11,-0x24DD(r12) (write valid flag)"),
    (0x034648, bytes([0x40, 0x82, 0x00, 0x60]), bytes([0x48, 0x00, 0x00, 0x60]),
     "HPT P1: credential bypass (bne -> b)"),
    (0x0346AC, bytes([0x40, 0x82, 0x00, 0x18]), bytes([0x60, 0x00, 0x00, 0x00]),
     "HPT P2: lock check bypass (bne -> nop)"),
    (0x034A70, bytes([0x40, 0x82, 0x00, 0x08]), bytes([0x48, 0x00, 0x00, 0x08]),
     "HPT P3: sig bypass (bne -> b)"),
]

# Per-OS overrides. Key = OS PN as int (read from 0x014638).
# Verified 2026-04-22 on stock bins and byte-for-byte code inspection.
OS_PATCHES = {
    # OS 24286985 — different compiler build, P1/P2/P3 shifted +0xE8/+0xE8/+0x120,
    # and USBJTAG P3a/P3b shifted +4 (cmpwi r10,0x80 at 0x02B398, bne at 0x02B39C).
    # Written v2 bin 2026-04-22 (CRC32 0x75FC6FB7, MD5 bdbc13fd…).
    24286985: [
        (0x02B3A0, bytes([0x4B, 0xFF, 0xFD, 0x11]), bytes([0x39, 0x60, 0x00, 0x01]),
         "USBJTAG P3a [985]: li r11,1 (set valid flag)"),
        (0x02B3A4, bytes([0x48, 0x00, 0x01, 0xC4]), bytes([0x99, 0x6C, 0xDB, 0x23]),
         "USBJTAG P3b [985]: stb r11,-0x24DD(r12) (write valid flag)"),
        (0x034730, bytes([0x40, 0x82, 0x00, 0x60]), bytes([0x48, 0x00, 0x00, 0x60]),
         "HPT P1 [985]: credential bypass (bne -> b)"),
        (0x034794, bytes([0x40, 0x82, 0x00, 0x18]), bytes([0x60, 0x00, 0x00, 0x00]),
         "HPT P2 [985]: lock check bypass (bne -> nop)"),
        (0x034B90, bytes([0x40, 0x82, 0x00, 0x08]), bytes([0x48, 0x00, 0x00, 0x08]),
         "HPT P3 [985]: sig bypass (bne -> b)"),
    ],
}


def get_patches_for_os(data):
    """Select the right patch table based on OS PN in the bin."""
    os_pn = struct.unpack('>I', data[OS_PN_OFFSET:OS_PN_OFFSET + 4])[0]
    if os_pn in OS_PATCHES:
        return OS_PATCHES[os_pn], os_pn, "per-OS"
    return DEFAULT_PATCHES, os_pn, "default-836"


# Backwards-compatible alias — some call sites still read PATCHES directly.
# This yields the default table; use get_patches_for_os(data) when you have
# the bin contents to pick the correct per-OS offsets.
PATCHES = DEFAULT_PATCHES

# ── Checksum Locations ──────────────────────────────────────────
# Boot Block segment (from UniversalPatcher t87a.xml)
CS1_ADDR = 0x028720  # CRC16-IBM, stored byte-swapped, 2 bytes
CS2_ADDR = 0x028700  # Wordsum BE, stored as two's complement, 2 bytes

# CS1 block ranges (start, end inclusive) — skip CS1 and CS2 storage locations
CS1_RANGES = [(0x020000, 0x0286FF), (0x028702, 0x02871F), (0x028722, 0x03FFFF)]
# CS2 block ranges — skip CS2 storage location only
CS2_RANGES = [(0x020000, 0x0286FF), (0x028702, 0x03FFFF)]

# OS PN location
OS_PN_OFFSET = 0x014638
# Cal ID (companion calibration part number, 4 bytes BE, immediately after OS PN)
CAL_ID_OFFSET = 0x01463C


def crc16_ibm(data_bytes):
    """CRC16-IBM (poly 0x8005, reflected/reversed)."""
    crc = 0x0000
    for b in data_bytes:
        crc ^= b
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def wordsum_be(data_bytes):
    """16-bit word sum, big-endian."""
    total = 0
    for i in range(0, len(data_bytes) - 1, 2):
        total += (data_bytes[i] << 8) | data_bytes[i + 1]
    return total & 0xFFFF


def calc_checksums(data):
    """Calculate Boot Block CS1 (CRC16) and CS2 (Wordsum)."""
    # CS1: CRC16-IBM over CS1_RANGES
    cs1_data = bytearray()
    for start, end in CS1_RANGES:
        cs1_data.extend(data[start:end + 1])
    cs1_raw = crc16_ibm(cs1_data)
    # Store byte-swapped
    cs1_stored = ((cs1_raw & 0xFF) << 8) | ((cs1_raw >> 8) & 0xFF)

    # CS2: Wordsum over CS2_RANGES, stored as two's complement
    cs2_data = bytearray()
    for start, end in CS2_RANGES:
        cs2_data.extend(data[start:end + 1])
    cs2_raw = wordsum_be(cs2_data)
    cs2_stored = (~cs2_raw + 1) & 0xFFFF

    return cs1_stored, cs2_stored


def apply_patches(data):
    """Apply 5-patch recipe. Returns patch_count.

    Selects per-OS offsets from OS_PATCHES if the OS PN is known, else
    falls back to the 24288836 default offsets.

    SAFETY: aborts if bytes at any target offset don't match the expected
    'old' pattern — compiler output for different OS versions places the
    credential/signature checks at different addresses. Writing the 'new'
    bytes blindly at fixed offsets in those OSes silently corrupts
    unrelated functions and produces broken bins that can brick a TCM.
    Proven 2026-04-22 on OS 24286985 where the real HPT check is at
    0x034730, not 0x034648 — added to OS_PATCHES so subsequent runs on
    985 bins now use the correct offsets.
    """
    patches, os_pn, source = get_patches_for_os(data)
    print(f"  Using {source} patch table for OS {os_pn}")
    patched = 0
    mismatches = []
    for offset, old, new, desc in patches:
        current = data[offset:offset + 4]
        if current == new:
            print(f"  [skip] {desc} — already patched")
            patched += 1
        elif current == old:
            data[offset:offset + 4] = new
            print(f"  [patch] {desc}")
            patched += 1
        else:
            mismatches.append((offset, desc, bytes(current).hex().upper(),
                               bytes(old).hex().upper()))
            print(f"  [ERR] {desc} @ 0x{offset:06X} — got 0x{current.hex().upper()}, "
                  f"expected 0x{old.hex().upper()}")
    if mismatches:
        print()
        print("ABORTING: patch offsets do not match this OS's compiler output.")
        print("Writing the patch anyway would corrupt unrelated functions and")
        print("produce a bin that may brick a TCM when flashed. This OS needs")
        print("per-OS offset re-validation — disassemble the bootloader around")
        print("the patched addresses to find the real credential / signature")
        print("check locations, then add an OS_PATCHES entry for this OS PN.")
        raise SystemExit(1)
    return patched


def update_checksums(data):
    """Recalculate and write Boot Block checksums.

    CS2 range includes the CS1 storage location (0x028720),
    so CS1 must be written FIRST, then CS2 recalculated.
    """
    old_cs1 = struct.unpack('>H', data[CS1_ADDR:CS1_ADDR + 2])[0]
    old_cs2 = struct.unpack('>H', data[CS2_ADDR:CS2_ADDR + 2])[0]

    # Step 1: Calculate and write CS1 (CRC16)
    cs1_data = bytearray()
    for start, end in CS1_RANGES:
        cs1_data.extend(data[start:end + 1])
    cs1_raw = crc16_ibm(cs1_data)
    cs1 = ((cs1_raw & 0xFF) << 8) | ((cs1_raw >> 8) & 0xFF)  # swap bytes
    struct.pack_into('>H', data, CS1_ADDR, cs1)

    # Step 2: Calculate and write CS2 (Wordsum) — AFTER CS1 is written
    cs2_data = bytearray()
    for start, end in CS2_RANGES:
        cs2_data.extend(data[start:end + 1])
    cs2_raw = wordsum_be(cs2_data)
    cs2 = (~cs2_raw + 1) & 0xFFFF  # two's complement
    struct.pack_into('>H', data, CS2_ADDR, cs2)

    print(f"  CS1 (CRC16)  @ 0x{CS1_ADDR:06X}: 0x{old_cs1:04X} -> 0x{cs1:04X}")
    print(f"  CS2 (Wordsum) @ 0x{CS2_ADDR:06X}: 0x{old_cs2:04X} -> 0x{cs2:04X}")
    return cs1, cs2


def verify_checksums(data):
    """Verify Boot Block checksums match calculated values."""
    cs1_calc, cs2_calc = calc_checksums(data)
    cs1_stored = struct.unpack('>H', data[CS1_ADDR:CS1_ADDR + 2])[0]
    cs2_stored = struct.unpack('>H', data[CS2_ADDR:CS2_ADDR + 2])[0]

    cs1_ok = cs1_calc == cs1_stored
    cs2_ok = cs2_calc == cs2_stored
    return cs1_ok, cs2_ok, cs1_stored, cs1_calc, cs2_stored, cs2_calc


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    if sys.argv[1] == '--verify':
        if len(sys.argv) < 3:
            print("Usage: t87a_patch.py --verify <file.bin>")
            sys.exit(1)
        path = sys.argv[2]
        with open(path, 'rb') as f:
            data = bytearray(f.read())

        print(f"File: {os.path.basename(path)} ({len(data):,} bytes)")
        os_pn = struct.unpack('>I', data[OS_PN_OFFSET:OS_PN_OFFSET + 4])[0]
        cal_id = struct.unpack('>I', data[CAL_ID_OFFSET:CAL_ID_OFFSET + 4])[0]
        file_crc = zlib.crc32(data) & 0xFFFFFFFF
        print(f"OS PN : {os_pn}   (@ 0x{OS_PN_OFFSET:06X})")
        print(f"Cal ID: {cal_id}   (@ 0x{CAL_ID_OFFSET:06X})")
        print(f"CRC32 : 0x{file_crc:08X}   (full 4 MB)")

        patches, _, source = get_patches_for_os(data)
        print(f"\nPatches ({source} table):")
        for offset, old, new, desc in patches:
            current = data[offset:offset + 4]
            if current == new:
                status = "PATCHED"
            elif current == old:
                status = "STOCK"
            else:
                status = f"OTHER (0x{current.hex().upper()})"
            print(f"  0x{offset:06X}: {status} — {desc}")

        print("\nChecksums:")
        cs1_ok, cs2_ok, cs1_s, cs1_c, cs2_s, cs2_c = verify_checksums(data)
        print(f"  CS1 stored=0x{cs1_s:04X} calc=0x{cs1_c:04X} {'OK' if cs1_ok else 'MISMATCH'}")
        print(f"  CS2 stored=0x{cs2_s:04X} calc=0x{cs2_c:04X} {'OK' if cs2_ok else 'MISMATCH'}")

        if cs1_ok and cs2_ok:
            print("\nAll checksums VALID.")
        else:
            print("\nChecksums INVALID — run without --verify to fix.")
        return

    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    no_bam = '--no-bam' in sys.argv

    input_path = args[0]
    output_path = args[1] if len(args) > 1 else None

    with open(input_path, 'rb') as f:
        data = bytearray(f.read())

    if len(data) != 4194304:
        print(f"ERROR: File must be 4,194,304 bytes (got {len(data):,})")
        sys.exit(1)

    if data[:4] != b'\x00\x5A\x00\x00':
        print("ERROR: Not a valid T87A flash file (missing RCHW header)")
        sys.exit(1)

    os_pn = struct.unpack('>I', data[OS_PN_OFFSET:OS_PN_OFFSET + 4])[0]
    cal_id = struct.unpack('>I', data[CAL_ID_OFFSET:CAL_ID_OFFSET + 4])[0]
    print(f"T87A 5-Patch Dual Unlock Tool")
    print(f"Input : {os.path.basename(input_path)}")
    print(f"OS PN : {os_pn}   (@ 0x{OS_PN_OFFSET:06X})")
    print(f"Cal ID: {cal_id}   (@ 0x{CAL_ID_OFFSET:06X})")

    print("\nApplying patches:")
    count = apply_patches(data)
    print(f"  {count}/5 patches applied")

    print("\nRecalculating checksums:")
    update_checksums(data)

    # Verify
    cs1_ok, cs2_ok, _, _, _, _ = verify_checksums(data)
    print(f"\nVerification: CS1={'OK' if cs1_ok else 'FAIL'} CS2={'OK' if cs2_ok else 'FAIL'}")

    if not output_path:
        base = os.path.splitext(os.path.basename(input_path))[0]
        output_path = os.path.join(os.path.dirname(input_path),
                                   f"{base}_5PATCH.bin")

    with open(output_path, 'wb') as f:
        f.write(data)
    print(f"\nSaved: {output_path}")
    print(f"CRC32: 0x{zlib.crc32(data) & 0xFFFFFFFF:08X}   (full 4 MB)")

    if not no_bam:
        bam_data = data[0x020000:] + bytes([0xFF] * 0x020000)
        bam_path = output_path.replace('.bin', '-BAM.bin')
        with open(bam_path, 'wb') as f:
            f.write(bam_data)
        print(f"BAM:   {bam_path}")


def batch_patch(directory):
    """Patch all 4MB T87A bins in a directory."""
    import glob
    bins = glob.glob(os.path.join(directory, '*.bin'))
    patched = 0
    for path in sorted(bins):
        if os.path.getsize(path) != 4194304:
            continue
        with open(path, 'rb') as f:
            hdr = f.read(4)
        if hdr != b'\x00\x5A\x00\x00':
            continue
        with open(path, 'rb') as f:
            data = bytearray(f.read())

        os_pn = struct.unpack('>I', data[OS_PN_OFFSET:OS_PN_OFFSET + 4])[0]

        # Skip already-patched files — use per-OS table to decide
        patches_for_this, _, _ = get_patches_for_os(data)
        already = all(data[off:off+4] == new for off, old, new, desc in patches_for_this)
        cs1_ok, cs2_ok, _, _, _, _ = verify_checksums(data)
        if already and cs1_ok and cs2_ok:
            print(f"  [skip] OS {os_pn} — already patched with valid checksums")
            continue

        apply_patches(data)
        update_checksums(data)
        cs1_ok, cs2_ok, _, _, _, _ = verify_checksums(data)

        base = os.path.splitext(os.path.basename(path))[0]
        out_dir = os.path.join(directory, 'patched')
        os.makedirs(out_dir, exist_ok=True)
        out_path = os.path.join(out_dir, f"T87A_OS-{os_pn}_5PATCH.bin")

        with open(out_path, 'wb') as f:
            f.write(data)

        status = "OK" if cs1_ok and cs2_ok else "CS FAIL"
        print(f"  [done] OS {os_pn} — {status} -> {os.path.basename(out_path)}")
        patched += 1

    print(f"\nBatch complete: {patched} files patched")


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--batch':
        if len(sys.argv) < 3:
            print("Usage: t87a_patch.py --batch <directory>")
            sys.exit(1)
        batch_patch(sys.argv[2])
    else:
        main()
