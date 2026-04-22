#!/usr/bin/env python3
"""
test_t87_write_kernel.py — Test T87 write kernel upload (NO erase, NO write)

Safe test: enters programming mode, uploads the calflash write kernel,
verifies it responds, then returns to normal. Nothing is written to flash.

Usage:
    python test_t87_write_kernel.py [COM13]
    python test_t87_write_kernel.py COM13 --full   # test full-flash kernel instead
"""

import serial
import sys
import time
import io
import os

# Add gm5byte to path for 5-byte seed key computation
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "gm5byte"))

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace", line_buffering=True)

port = "COM13"
use_full = False
for arg in sys.argv[1:]:
    if arg.startswith("COM") or arg.startswith("/dev/"):
        port = arg
    elif arg == "--full":
        use_full = True


def send(ser, cmd, timeout=10):
    """Send command, print all output until OK/ERR or timeout."""
    print(f"\n  > {cmd}")
    ser.write((cmd + "\r\n").encode())
    time.sleep(0.05)
    deadline = time.time() + timeout
    lines = []
    while time.time() < deadline:
        line = ser.readline().decode("ascii", errors="replace").strip()
        if not line:
            continue
        print(f"  < {line}")
        lines.append(line)
        if line == "OK" or line.startswith("ERR:"):
            return lines
    return lines


def send_raw(ser, cmd, wait=0.1):
    """Send command, don't wait for OK."""
    print(f"\n  > {cmd}")
    ser.write((cmd + "\r\n").encode())
    time.sleep(wait)


def drain(ser, timeout=2):
    """Read and print all available output."""
    deadline = time.time() + timeout
    lines = []
    while time.time() < deadline:
        line = ser.readline().decode("ascii", errors="replace").strip()
        if not line:
            continue
        print(f"  < {line}")
        lines.append(line)
    return lines


print(f"=== T87 Write Kernel Upload Test ===")
print(f"Port: {port}")
print(f"Kernel: {'full-flash' if use_full else 'calflash'}")
print(f"This test is SAFE — no flash erase or write occurs.\n")

ser = serial.Serial(port, 115200, timeout=1)
time.sleep(2)

# Drain boot messages
while ser.in_waiting:
    line = ser.readline().decode("ascii", errors="replace").strip()
    if line:
        print(f"  < {line}")

# Step 1: Init CAN
send(ser, "INIT")

# Step 2: Set module and CAN IDs
send(ser, "ALGO T87")
send(ser, "SETID 7E2 7EA")

# Step 3: $20 reset any prior session
print("\n--- Step 1: Reset prior session ---")
send(ser, "CANSEND 101 FE 01 20 00 00 00 00 00")
time.sleep(0.2)

# Step 4: Read VIN (confirms we can talk to TCM in default session)
print("\n--- Step 2: Read VIN ---")
send(ser, "VIN")

# Step 5: Enable broadcast TesterPresent
send(ser, "BUSTP on")

# Step 6: $28 disableNormalComm (BEFORE SecurityAccess)
print("\n--- Step 3: Disable normal comms ---")
send(ser, "CANSEND 101 FE 01 28 00 00 00 00 00")
time.sleep(0.1)

# Step 7: SecurityAccess (AFTER $28, BEFORE $A5)
print("\n--- Step 4: SecurityAccess ---")
lines = send(ser, "AUTH", timeout=10)
# Check for zero seed (already unlocked) or 2-byte unlock
seed_zero = any("0000000000" in l for l in lines)
auth_ok = any("unlocked" in l.lower() or "Security unlocked" in l for l in lines) or seed_zero
if seed_zero:
    print("  Seed is all zeros — already unlocked (no key needed)")

# If not yet unlocked, check for 5-byte seed and compute key
if not auth_ok:
    seed_hex = None
    for l in lines:
        if "SEED:" in l:
            # Extract hex after "SEED:" — e.g. "SEED: F118278806" or "SEED:F118278806"
            parts = l.split("SEED:")
            if len(parts) > 1:
                seed_hex = parts[1].strip().replace(" ", "")
    if seed_hex and len(seed_hex) == 10:
        print(f"  5-byte seed detected: {seed_hex}")
        try:
            from keylib import derive_key_from_algo
            seed_bytes = bytes.fromhex(seed_hex)
            mac, iterations, aes_key = derive_key_from_algo(135, seed_bytes)
            key_hex = mac.hex().upper()
            print(f"  Computed 5-byte key: {key_hex}")
            lines2 = send(ser, f"AUTH {key_hex}", timeout=10)
            auth_ok = any("unlocked" in l.lower() or "Security unlocked" in l for l in lines2)
        except Exception as e:
            print(f"  5-byte key computation failed: {e}")

if not auth_ok:
    print("\n*** SecurityAccess FAILED — cannot proceed ***")
    ser.close()
    sys.exit(1)

# Step 8: $A5 01/$A5 03 enter programming mode (AFTER SecurityAccess)
print("\n--- Step 5: Enter programming mode ---")

# $A5 01 requestProgrammingMode
send(ser, "CANSEND 101 FE 02 A5 01 00 00 00 00")
time.sleep(0.3)

# $A5 03 enableProgrammingMode (TCM reboots to bootloader)
print("  Sending $A5 03 — TCM will reboot to bootloader...")
send(ser, "CANSEND 101 FE 02 A5 03 00 00 00 00")
time.sleep(0.5)

# Send a couple TesterPresent broadcasts to keep bus alive
send(ser, "CANSEND 101 FE 01 3E 00 00 00 00 00")
time.sleep(0.05)
send(ser, "CANSEND 101 FE 01 3E 00 00 00 00 00")
time.sleep(0.1)

# Step 8: Upload write kernel
print("\n--- Step 6: Upload write kernel ---")
kernel_arg = "write_full" if use_full else "write"
lines = send(ser, f"KERNEL {kernel_arg}", timeout=30)

kernel_ok = any("running" in l.lower() or "Kernel running" in l for l in lines)
if kernel_ok:
    print("\n*** WRITE KERNEL UPLOADED SUCCESSFULLY ***")
else:
    print("\n*** Kernel upload result — check output above ***")
    # It might still have worked — look for RequestDownload accepted + TransferData
    for l in lines:
        if "accepted" in l.lower() or "KERNEL:" in l:
            print(f"  Key line: {l}")

# Step 9: Probe the running kernel — try some safe queries
print("\n--- Step 7: Probe kernel (safe queries) ---")

# $1A B4 — read OSID/calibration ID
send_raw(ser, "RAW 1A B4")
time.sleep(0.5)
drain(ser, 2)

# $1A C1 — read app SW version (kernel CVN)
send_raw(ser, "RAW 1A C1")
time.sleep(0.5)
drain(ser, 2)

# $3E TesterPresent
send_raw(ser, "RAW 3E")
time.sleep(0.5)
drain(ser, 2)

# $1A 55 — read FNA (functional network address)
send_raw(ser, "RAW 1A 55")
time.sleep(0.5)
drain(ser, 2)

# $36 FF — finalize (safe probe: no prior erase, tests $36 communication path)
print("\n--- Step 7b: $36 FF finalize probe (no flash modification) ---")
send_raw(ser, "RAW 36 FF")
time.sleep(1.0)
drain(ser, 3)

# Step 10: Return to normal — safe shutdown
print("\n--- Step 8: Return to normal ---")

# $20 broadcast — returnToNormalMode
send(ser, "CANSEND 101 FE 01 20 00 00 00 00 00")
time.sleep(0.2)

# $14 broadcast — clearDTCs
send(ser, "CANSEND 101 FE 01 14 00 00 00 00 00")
time.sleep(0.1)

# Disable broadcast TP
send(ser, "BUSTP off")

print("\n=== Test complete ===")
print("TCM should be back in normal mode.")
print("Power cycle the TCM if it doesn't respond to PING.")

# Final PING to check if TCM recovered
time.sleep(1)
send(ser, "PING")

ser.close()
