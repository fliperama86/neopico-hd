#!/usr/bin/env python3
"""
Monitor serial output for FIFO diagnostic messages.
"""

import serial
import serial.tools.list_ports
import sys
import time

def find_pico_port():
    """Auto-detect Pico CDC port."""
    ports = serial.tools.list_ports.comports()
    for port in ports:
        if 'ACM' in port.device or 'usbmodem' in port.device:
            return port.device
    return None

def monitor_serial():
    """Monitor serial output and highlight FIFO drainage messages."""
    port = find_pico_port()
    if not port:
        print("Error: Could not find Pico serial port", file=sys.stderr)
        sys.exit(1)

    print(f"Connecting to {port}...")
    print("Monitoring FIFO diagnostics (Ctrl+C to stop)...")
    print("=" * 60)

    try:
        ser = serial.Serial(port, 115200, timeout=1)

        while True:
            if ser.in_waiting:
                line = ser.readline().decode('utf-8', errors='replace').strip()
                if line:
                    # Highlight FIFO drainage messages
                    if "Drained FIFO" in line:
                        print(f"\n🔴 {line}\n")
                    elif "Frame" in line or "vsync" in line:
                        print(f"📊 {line}")
                    else:
                        print(f"   {line}")
            else:
                time.sleep(0.01)

    except KeyboardInterrupt:
        print("\n\nMonitoring stopped.")
        ser.close()
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    monitor_serial()
