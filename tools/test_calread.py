#!/usr/bin/env python3
"""Test partial reads: NVM/adaptation region vs calibration region on T87."""
import serial
import sys
import time
import io
import re

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace", line_buffering=True)

port = "COM13"
module = "T87"
for arg in sys.argv[1:]:
    if arg.startswith("COM") or arg.startswith("/dev/"):
        port = arg
baud = 115200

# T87 flash regions from A2L analysis:
#   NVM/Adaptation: 0x008000 - 0x07FFFF  (480KB, 240 blocks)
#   Calibration:    0x080000 - 0x17FFFF  (1MB, 512 blocks)
# READ command takes: READ <start_addr_hex> <block_count_decimal>
REGIONS = {
    "NVM":  ("008000", "240"),    # 240 blocks x 0x800 = 480KB
    "CAL":  ("080000", "512"),    # 512 blocks x 0x800 = 1MB
}


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
        # Only print progress lines at milestones, not every block
        if line.startswith("DATA:"):
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


def enter_programming_mode(ser):
    """Manual programming mode entry (same sequence as FULLREAD)."""
    send_and_wait(ser, "INIT")
    send_and_wait(ser, "ALGO T87")
    send_and_wait(ser, "SETID 7E2 7EA")

    # returnToNormalMode — reset prior session
    send_and_wait(ser, "CANSEND 101 FE 01 20 00 00 00 00 00")
    time.sleep(0.2)

    # Read VIN + OSID for identification
    send_and_wait(ser, "RAW 1A 90")   # VIN
    send_and_wait(ser, "RAW 1A B4")   # Serial/OSID

    # disableNormalCommunication
    send_and_wait(ser, "CANSEND 101 FE 01 28 00 00 00 00 00")
    time.sleep(0.1)

    # SecurityAccess
    send_and_wait(ser, "AUTH")

    # requestProgrammingMode
    send_and_wait(ser, "CANSEND 101 FE 02 A5 01 00 00 00 00")
    time.sleep(0.3)

    # enableProgrammingMode (TCM reboots, no response)
    send_and_wait(ser, "CANSEND 101 FE 02 A5 03 00 00 00 00")
    time.sleep(0.3)

    # Upload + execute kernel
    send_and_wait(ser, "KERNEL", timeout=30)
    return True


def exit_programming_mode(ser):
    """Return to normal mode."""
    send_and_wait(ser, "CANSEND 101 FE 01 20 00 00 00 00 00")
    time.sleep(0.5)


print(f"Opening {port} at {baud} baud...")
ser = serial.Serial(port, baud, timeout=1)
time.sleep(2)
while ser.in_waiting:
    print("  <", ser.readline().decode("ascii", errors="replace").strip())

# --- Enter programming mode ---
print("\n" + "=" * 50)
print("  Entering programming mode...")
print("=" * 50)
if not enter_programming_mode(ser):
    print("\n*** Failed to enter programming mode ***")
    ser.close()
    sys.exit(1)

# --- Read NVM/Adaptation ---
print("\n" + "=" * 50)
print("  NVM/Adaptation: 0x008000 - 0x07FFFF (480KB, 240 blocks)")
print("=" * 50)
addr, size = REGIONS["NVM"]
cs_nvm = send_and_wait(ser, f"READ {addr} {size}", done_marker="READ:DONE", timeout=120)
print(f"\n  NVM checksum: {cs_nvm}")

# --- Read Calibration ---
print("\n" + "=" * 50)
print("  Calibration: 0x080000 - 0x17FFFF (1MB, 512 blocks)")
print("=" * 50)
addr, size = REGIONS["CAL"]
cs_cal = send_and_wait(ser, f"READ {addr} {size}", done_marker="READ:DONE", timeout=240)
print(f"\n  CAL checksum: {cs_cal}")

# --- Exit programming mode ---
print("\n" + "=" * 50)
print("  Exiting programming mode...")
exit_programming_mode(ser)

# --- Summary ---
print("\n" + "=" * 50)
print("  RESULTS")
print("=" * 50)
print(f"  NVM/Adaptation (0x008000, 480KB): checksum={cs_nvm}")
print(f"  Calibration    (0x080000, 1MB):   checksum={cs_cal}")
print(f"  Combined would be 752 blocks vs 2048 full read")
print("=" * 50)

send_and_wait(ser, "LEDPARTY")
ser.close()
