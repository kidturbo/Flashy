#!/usr/bin/env python3
"""
detect_port.py — Auto-detect Adafruit Feather M4 CAN serial port.

Usage as module:
    from detect_port import find_feather
    port = find_feather()  # returns "COM38" or None

Usage standalone:
    python detect_port.py          # prints detected port or error
"""

import serial.tools.list_ports

ADAFRUIT_VID = 0x239A  # Adafruit Industries USB Vendor ID


def find_feather():
    """Return the COM port string for the first Adafruit device found, or None."""
    for p in serial.tools.list_ports.comports():
        if p.vid == ADAFRUIT_VID:
            return p.device
    return None


def find_feather_or_prompt():
    """Auto-detect Feather port; fall back to manual input if not found."""
    port = find_feather()
    if port:
        print(f"  Auto-detected Feather on {port}")
        return port
    print("  Could not auto-detect Feather.")
    port = input("  Enter COM port manually (e.g. COM38): ").strip()
    return port if port else None


if __name__ == '__main__':
    port = find_feather()
    if port:
        print(f"Feather detected on {port}")
    else:
        print("No Adafruit device found.")
        print("Available ports:")
        for p in serial.tools.list_ports.comports():
            print(f"  {p.device}  VID={p.vid:#06x if p.vid else 'N/A'}  {p.description}")
