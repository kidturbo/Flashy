#!/usr/bin/env python3
"""Quick test: Does FULLREAD work on this T87A? Just sends FULLREAD and watches.
Kill with Ctrl+C once you see kernel upload result (no need to wait for full read)."""
import serial, sys, time, io

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace", line_buffering=True)

port = sys.argv[1] if len(sys.argv) > 1 else "COM13"
ser = serial.Serial(port, 115200, timeout=1)
time.sleep(2)

# Drain boot messages
while ser.in_waiting:
    line = ser.readline().decode("ascii", errors="replace").strip()
    if line: print(f"  < {line}")

def send(cmd, timeout=60):
    print(f"\n  > {cmd}")
    ser.write((cmd + "\r\n").encode())
    time.sleep(0.05)
    deadline = time.time() + timeout
    lines = []
    while time.time() < deadline:
        line = ser.readline().decode("ascii", errors="replace").strip()
        if not line: continue
        print(f"  < {line}")
        lines.append(line)
        if line == "OK" or line.startswith("ERR:"):
            return lines
        if "FULLREAD:DONE" in line:
            return lines
    return lines

send("INIT")
send("ALGO T87")
send("SETID 7E2 7EA")

print("\n=== Starting FULLREAD (watching for kernel upload result) ===")
print("Press Ctrl+C once you see 'RequestDownload' or 'Kernel running'\n")

# Send FULLREAD and watch everything
ser.write(b"FULLREAD\r\n")
try:
    while True:
        line = ser.readline().decode("ascii", errors="replace").strip()
        if line:
            print(f"  < {line}")
            if "FULLREAD:DONE" in line or line == "OK":
                break
            if "kernel upload failed" in line.lower():
                print("\n*** KERNEL UPLOAD FAILED ***")
                break
            if "DATA:" in line:
                print("  (data flowing — kernel upload worked! Kill with Ctrl+C)")
except KeyboardInterrupt:
    print("\n\nAborted by user.")

ser.close()
