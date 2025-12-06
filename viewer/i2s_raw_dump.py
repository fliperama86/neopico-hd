#!/usr/bin/env python3
"""
Raw USB dump - see exactly what's coming from the Pico
And try to parse packets manually
"""

import sys
import struct
import serial
import serial.tools.list_ports

def find_pico_port():
    ports = serial.tools.list_ports.comports()
    for port in ports:
        if 'usbmodem' in port.device or port.vid == 0x2E8A:
            return port.device
    return None

def main():
    port = find_pico_port()
    if not port:
        print("No Pico found")
        sys.exit(1)

    print(f"Reading from {port}...")
    ser = serial.Serial(port, 115200, timeout=0.5)
    ser.reset_input_buffer()

    # Read 4KB
    print("Reading 4KB of data...\n")
    data = ser.read(4096)
    print(f"Got {len(data)} bytes\n")

    # Find and parse all packets
    sync = bytes([0xDE, 0xAD, 0xBE, 0xEF])
    pos = 0
    packets = []

    while True:
        idx = data.find(sync, pos)
        if idx == -1:
            break

        # Found sync at idx
        if idx + 6 <= len(data):
            count = struct.unpack('<H', data[idx+4:idx+6])[0]
            data_size = count * 4
            packet_end = idx + 6 + data_size

            if packet_end <= len(data):
                packet_data = data[idx+6:packet_end]
                # Parse first few samples
                samples = struct.unpack(f'<{min(8, count*2)}h', packet_data[:min(16, len(packet_data))])
                packets.append({
                    'offset': idx,
                    'count': count,
                    'samples': samples,
                    'data_len': len(packet_data)
                })

        pos = idx + 1

    print(f"Found {len(packets)} complete packets:\n")
    for i, p in enumerate(packets[:10]):  # Show first 10
        print(f"Packet {i}: offset=0x{p['offset']:04X}, count={p['count']}, samples={p['samples'][:4]}...")

    # Show sample value distribution
    if packets:
        all_samples = []
        for p in packets:
            all_samples.extend(p['samples'])
        if all_samples:
            print(f"\nSample stats: min={min(all_samples)}, max={max(all_samples)}, count={len(all_samples)}")

    ser.close()

if __name__ == '__main__':
    main()
