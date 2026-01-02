#!/usr/bin/env python3
"""
Reset Pico and monitor serial output.
"""

import serial
import subprocess
import sys
import time

DEVICE = "/dev/tty.usbmodem21401"
BAUD = 115200

def main():
    timeout_sec = int(sys.argv[1]) if len(sys.argv) > 1 else 15

    # First, reset the device
    print("Resetting device...")
    subprocess.run(["picotool", "reboot", "-f"], capture_output=True)

    # Wait for USB to re-enumerate
    print("Waiting for USB to re-enumerate...")
    time.sleep(3)

    # Now open serial
    print(f"Opening {DEVICE}...")
    try:
        ser = serial.Serial(DEVICE, BAUD, timeout=0.5)
    except serial.SerialException as e:
        print(f"Error: {e}")
        # Try to find alternate device
        import glob
        devices = glob.glob("/dev/tty.usbmodem*") + glob.glob("/dev/cu.usbmodem*")
        print(f"Available devices: {devices}")
        return 1

    print(f"Monitoring for {timeout_sec}s...\n")
    print("=" * 60)

    start_time = time.time()
    lines = []

    while (time.time() - start_time) < timeout_sec:
        try:
            if ser.in_waiting:
                line = ser.readline().decode('utf-8', errors='replace').rstrip()
                if line:
                    print(line)
                    lines.append(line)
        except Exception as e:
            print(f"[Error: {e}]")
        time.sleep(0.01)

    ser.close()
    print("=" * 60)
    print(f"Captured {len(lines)} lines")
    return 0

if __name__ == "__main__":
    sys.exit(main())
