#!/usr/bin/env python3
"""
Serial monitor with timeout for RP2350 debugging.
Usage: python3 serial_monitor.py [timeout_seconds]
"""

import serial
import sys
import time

DEVICE = "/dev/tty.usbmodem21401"
BAUD = 115200
DEFAULT_TIMEOUT = 10

def monitor(timeout_sec):
    print(f"Opening {DEVICE} at {BAUD} baud, timeout={timeout_sec}s")

    try:
        ser = serial.Serial(DEVICE, BAUD, timeout=1)
    except serial.SerialException as e:
        print(f"Error opening serial port: {e}")
        print("Is the device connected and not in bootloader mode?")
        return 1

    print("Monitoring... (Ctrl+C to stop early)\n")
    print("=" * 60)

    start_time = time.time()
    lines_captured = 0

    try:
        while (time.time() - start_time) < timeout_sec:
            if ser.in_waiting:
                try:
                    line = ser.readline().decode('utf-8', errors='replace').rstrip()
                    if line:
                        print(line)
                        lines_captured += 1
                except Exception as e:
                    print(f"[decode error: {e}]")
            else:
                time.sleep(0.01)
    except KeyboardInterrupt:
        print("\n[Interrupted by user]")
    finally:
        ser.close()

    print("=" * 60)
    print(f"Captured {lines_captured} lines in {time.time() - start_time:.1f}s")
    return 0

if __name__ == "__main__":
    timeout = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_TIMEOUT
    sys.exit(monitor(timeout))
