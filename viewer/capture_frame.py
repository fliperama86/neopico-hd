#!/usr/bin/env python3
"""
Capture one frame from Pico and save as PNG for analysis.
"""

import sys
import serial
import serial.tools.list_ports
from PIL import Image

# Frame constants
FRAME_WIDTH = 320
FRAME_HEIGHT = 224
FRAME_SIZE_BYTES = FRAME_WIDTH * FRAME_HEIGHT * 2  # RGB565

# Sync header
SYNC_HEADER = bytes([0x55, 0xAA, 0xAA, 0x55])


def find_pico_port():
    """Auto-detect Pico CDC port."""
    ports = serial.tools.list_ports.comports()
    for port in ports:
        if 'ACM' in port.device or 'usbmodem' in port.device:
            return port.device
    return None


def sync_to_frame(ser):
    """Find frame sync header in stream."""
    buffer = bytearray(4)
    sync_bytes = 0
    while sync_bytes < 1000000:
        byte = ser.read(1)
        if not byte:
            continue
        buffer.pop(0)
        buffer.append(byte[0])
        sync_bytes += 1
        if bytes(buffer) == SYNC_HEADER:
            return True
    return False


def read_frame(ser):
    """Read a complete frame."""
    frame_data = ser.read(FRAME_SIZE_BYTES)
    if len(frame_data) != FRAME_SIZE_BYTES:
        return None
    return frame_data


def unpack_frame_to_image(frame_data):
    """Unpack RGB565 pixels to PIL Image."""
    img = Image.new('RGB', (FRAME_WIDTH, FRAME_HEIGHT))
    pixels = []

    for i in range(0, len(frame_data), 2):
        # RGB565: RRRRRGGG GGGBBBBB (little-endian)
        lo = frame_data[i]
        hi = frame_data[i + 1]
        rgb565 = lo | (hi << 8)

        # Extract components
        r5 = (rgb565 >> 11) & 0x1F
        g6 = (rgb565 >> 5) & 0x3F
        b5 = rgb565 & 0x1F

        # Expand to 8-bit
        r8 = (r5 << 3) | (r5 >> 2)
        g8 = (g6 << 2) | (g6 >> 4)
        b8 = (b5 << 3) | (b5 >> 2)

        pixels.append((r8, g8, b8))

    img.putdata(pixels)
    return img


def main():
    output_file = sys.argv[1] if len(sys.argv) > 1 else '/tmp/pico_frame.png'

    # Find port
    port = find_pico_port()
    if not port:
        print("Error: Could not find Pico serial port", file=sys.stderr)
        sys.exit(1)

    print(f"Connecting to {port}...", file=sys.stderr)

    try:
        ser = serial.Serial(port, 115200, timeout=2)
    except serial.SerialException as e:
        print(f"Error opening port: {e}", file=sys.stderr)
        sys.exit(1)

    print("Waiting for frame sync...", file=sys.stderr)
    if not sync_to_frame(ser):
        print("Error: Could not sync to frame", file=sys.stderr)
        ser.close()
        sys.exit(1)

    print("Reading frame...", file=sys.stderr)
    frame_data = read_frame(ser)
    ser.close()

    if frame_data is None:
        print("Error: Failed to read complete frame", file=sys.stderr)
        sys.exit(1)

    print("Unpacking and saving...", file=sys.stderr)
    img = unpack_frame_to_image(frame_data)
    img.save(output_file)

    print(f"Frame saved to {output_file}", file=sys.stderr)
    print(output_file)  # Print path to stdout for easy capture


if __name__ == '__main__':
    main()
