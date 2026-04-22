#!/usr/bin/env python3
"""FULLREAD test driver — double read with checksum verification (no power cycle)."""
import serial
import sys
import time
import io
import re

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace", line_buffering=True)

# Usage: test_read.py [port] [module]
#   module: T87, E38N, E38, T42, E92 (default: T87)
port = "COM13"
module = "T87"
for arg in sys.argv[1:]:
    if arg.upper() in ("T87", "E38N", "E38", "T42", "E92"):
        module = arg.upper()
    elif arg.startswith("COM") or arg.startswith("/dev/"):
        port = arg
baud = 115200


def send_and_wait(ser, cmd, done_marker=None, timeout=300):
    """Send a command, print output, return all lines. Stops at done_marker or OK/ERR."""
    checksum = None
    print(f"\n  > {cmd}")
    ser.write((cmd + "\r\n").encode())
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = ser.readline().decode("ascii", errors="replace").strip()
        if not line:
            continue
        print(f"  < {line}")

        m = re.search(r"checksum=(0x[0-9A-Fa-f]+)", line)
        if m:
            checksum = m.group(1)

        if done_marker and done_marker in line:
            time.sleep(0.3)
            while ser.in_waiting:
                extra = ser.readline().decode("ascii", errors="replace").strip()
                if extra:
                    print(f"  < {extra}")
            return checksum
        if line == "OK" and not done_marker:
            return checksum
        if line.startswith("ERR:"):
            return None
    return None


# Module-specific CAN IDs (tester TX, ECU RX)
MODULE_CAN_IDS = {
    "T87":  ("7E2", "7EA"),   # TCM
    "E38":  ("7E0", "7E8"),   # ECM
    "E38N": ("7E0", "7E8"),   # ECM (external-kernel variant)
    "T42":  ("7E2", "7EA"),   # TCM
    "E92":  ("7E0", "7E8"),   # ECM
}

read_timeout = 600 if module in ("T87",) else 300  # T87=4MB, E38=2MB

print(f"Opening {port} at {baud} baud — module {module}...")
ser = serial.Serial(port, baud, timeout=1)
time.sleep(2)
while ser.in_waiting:
    print("  <", ser.readline().decode("ascii", errors="replace").strip())

# --- Read 1: full entry sequence ---
print("\n" + "=" * 50)
print(f"  READ 1 — {module} full entry sequence")
print("=" * 50)
send_and_wait(ser, "INIT")
send_and_wait(ser, f"ALGO {module}")
tx_id, rx_id = MODULE_CAN_IDS.get(module, ("7E0", "7E8"))
send_and_wait(ser, f"SETID {tx_id} {rx_id}")
cs1 = send_and_wait(ser, "FULLREAD", done_marker="FULLREAD:DONE", timeout=read_timeout)
if not cs1:
    print("\n*** READ 1 FAILED ***")
    ser.close()
    sys.exit(1)
print(f"\n  Read 1 checksum: {cs1}")

# --- Wait for ECU to fully restart (orange/yellow fade on LED) ---
print("\n  Waiting for ECU to restart...")
send_and_wait(ser, "LEDWAIT 30000")

# --- Read 2: full sequence again ---
print("\n" + "=" * 50)
print(f"  READ 2 — {module} verification")
print("=" * 50)
send_and_wait(ser, "INIT")
send_and_wait(ser, f"ALGO {module}")
send_and_wait(ser, f"SETID {tx_id} {rx_id}")
cs2 = send_and_wait(ser, "FULLREAD", done_marker="FULLREAD:DONE", timeout=read_timeout)
if not cs2:
    print("\n*** READ 2 FAILED ***")
    ser.close()
    sys.exit(1)
print(f"\n  Read 2 checksum: {cs2}")

# --- Compare ---
print("\n" + "=" * 50)
if cs1 == cs2:
    print(f"  MATCH! Both reads: {cs1}")
    send_and_wait(ser, "LEDPARTY")
else:
    print(f"  MISMATCH! Read 1: {cs1}  Read 2: {cs2}")
print("=" * 50)

ser.close()
