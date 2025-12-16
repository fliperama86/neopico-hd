#!/usr/bin/env python3
"""
YM2610 Audio Logic Analyzer Viewer

Connects to Pico logic analyzer and displays BCK/DAT/WS waveforms.

Usage:
    python viewer.py [serial_port]

Example:
    python viewer.py /dev/tty.usbmodem*
"""

import sys
import struct
import serial
import serial.tools.list_ports
import matplotlib.pyplot as plt
import numpy as np
from pathlib import Path
import argparse

def find_pico_port():
    """Find the Pico's USB serial port."""
    ports = list(serial.tools.list_ports.comports())
    for port in ports:
        if 'usbmodem' in port.device.lower() or 'usb' in port.device.lower():
            print(f"Found potential Pico port: {port.device}")
            return port.device
    return None

def capture_data(ser):
    """Send capture command and receive binary data."""
    print("Sending capture command...")
    ser.write(b'B')  # Binary stream mode

    # Read until we find the magic header
    print("Waiting for data header...")
    while True:
        header = ser.read(4)
        if header == b'LOGA':
            break
        # Keep looking
        if len(header) == 4:
            ser.read(1)  # shift by one byte and try again

    print("Found header, reading metadata...")

    # Read metadata
    sample_rate = struct.unpack('<I', ser.read(4))[0]
    num_samples = struct.unpack('<I', ser.read(4))[0]
    pin_bck = ser.read(1)[0]
    pin_dat = ser.read(1)[0]
    pin_ws = ser.read(1)[0]
    _ = ser.read(1)  # reserved

    print(f"Sample rate: {sample_rate / 1e6:.1f} MHz")
    print(f"Num samples: {num_samples}")
    print(f"Pins: BCK={pin_bck}, DAT={pin_dat}, WS={pin_ws}")

    # Read sample data
    print(f"Reading {num_samples} samples...")
    data = ser.read(num_samples)
    if len(data) != num_samples:
        print(f"Warning: only got {len(data)} samples")

    # Unpack into arrays
    bck = np.array([(b & 0x01) for b in data], dtype=np.uint8)
    dat = np.array([(b & 0x02) >> 1 for b in data], dtype=np.uint8)
    ws = np.array([(b & 0x04) >> 2 for b in data], dtype=np.uint8)

    return {
        'sample_rate': sample_rate,
        'bck': bck,
        'dat': dat,
        'ws': ws,
        'pin_bck': pin_bck,
        'pin_dat': pin_dat,
        'pin_ws': pin_ws
    }

def analyze_signal(data, name, sample_rate):
    """Analyze a digital signal and return statistics."""
    # Find transitions
    transitions = np.sum(np.abs(np.diff(data)))
    cycles = transitions // 2
    duration_s = len(data) / sample_rate
    frequency = cycles / duration_s

    # Duty cycle
    duty = np.mean(data) * 100

    return {
        'transitions': transitions,
        'cycles': cycles,
        'frequency': frequency,
        'duty': duty
    }

def plot_signals(capture, output_file=None, zoom_samples=500):
    """Plot the captured signals."""
    sample_rate = capture['sample_rate']
    bck = capture['bck']
    dat = capture['dat']
    ws = capture['ws']

    # Create time axis in microseconds
    num_samples = len(bck)
    time_us = np.arange(num_samples) / (sample_rate / 1e6)

    # Analyze signals
    bck_stats = analyze_signal(bck, 'BCK', sample_rate)
    dat_stats = analyze_signal(dat, 'DAT', sample_rate)
    ws_stats = analyze_signal(ws, 'WS', sample_rate)

    # Create figure with two views: full and zoomed
    fig, axes = plt.subplots(3, 2, figsize=(14, 8))
    fig.suptitle('YM2610 Audio Signals', fontsize=14)

    # Zoomed view (first zoom_samples)
    zoom_time = time_us[:zoom_samples]
    zoom_bck = bck[:zoom_samples]
    zoom_dat = dat[:zoom_samples]
    zoom_ws = ws[:zoom_samples]

    # BCK signal
    axes[0, 0].step(zoom_time, zoom_bck + 0.05, where='post', linewidth=0.8, color='blue')
    axes[0, 0].set_ylim(-0.1, 1.2)
    axes[0, 0].set_ylabel(f'BCK (GP{capture["pin_bck"]})')
    axes[0, 0].set_title(f'BCK: {bck_stats["frequency"]/1000:.1f} kHz (expected: ~1776 kHz)')
    axes[0, 0].grid(True, alpha=0.3)

    # DAT signal
    axes[1, 0].step(zoom_time, zoom_dat + 0.05, where='post', linewidth=0.8, color='green')
    axes[1, 0].set_ylim(-0.1, 1.2)
    axes[1, 0].set_ylabel(f'DAT (GP{capture["pin_dat"]})')
    axes[1, 0].set_title(f'DAT: {dat_stats["transitions"]} transitions ({dat_stats["duty"]:.1f}% high)')
    axes[1, 0].grid(True, alpha=0.3)

    # WS signal
    axes[2, 0].step(zoom_time, zoom_ws + 0.05, where='post', linewidth=0.8, color='red')
    axes[2, 0].set_ylim(-0.1, 1.2)
    axes[2, 0].set_ylabel(f'WS (GP{capture["pin_ws"]})')
    axes[2, 0].set_title(f'WS: {ws_stats["frequency"]/1000:.1f} kHz (expected: ~55.5 kHz)')
    axes[2, 0].set_xlabel('Time (μs)')
    axes[2, 0].grid(True, alpha=0.3)

    # Full capture overview (downsampled for display)
    downsample = max(1, num_samples // 5000)
    full_time = time_us[::downsample]
    full_bck = bck[::downsample]
    full_dat = dat[::downsample]
    full_ws = ws[::downsample]

    axes[0, 1].plot(full_time / 1000, full_bck, linewidth=0.3, color='blue', alpha=0.7)
    axes[0, 1].set_ylim(-0.1, 1.2)
    axes[0, 1].set_title('Full Capture - BCK')
    axes[0, 1].grid(True, alpha=0.3)

    axes[1, 1].plot(full_time / 1000, full_dat, linewidth=0.3, color='green', alpha=0.7)
    axes[1, 1].set_ylim(-0.1, 1.2)
    axes[1, 1].set_title('Full Capture - DAT')
    axes[1, 1].grid(True, alpha=0.3)

    axes[2, 1].plot(full_time / 1000, full_ws, linewidth=0.3, color='red', alpha=0.7)
    axes[2, 1].set_ylim(-0.1, 1.2)
    axes[2, 1].set_xlabel('Time (ms)')
    axes[2, 1].set_title('Full Capture - WS')
    axes[2, 1].grid(True, alpha=0.3)

    plt.tight_layout()

    if output_file:
        plt.savefig(output_file, dpi=150)
        print(f"Saved plot to {output_file}")
    else:
        plt.show()

def print_analysis(capture):
    """Print detailed signal analysis."""
    sample_rate = capture['sample_rate']

    print("\n" + "=" * 60)
    print("Signal Analysis")
    print("=" * 60)

    for name, data, pin, expected in [
        ('BCK', capture['bck'], capture['pin_bck'], 1776000),
        ('DAT', capture['dat'], capture['pin_dat'], None),
        ('WS', capture['ws'], capture['pin_ws'], 55500)
    ]:
        stats = analyze_signal(data, name, sample_rate)
        print(f"\n{name} (GPIO {pin}):")
        print(f"  Transitions: {stats['transitions']}")
        print(f"  Frequency:   {stats['frequency']/1000:.1f} kHz")
        print(f"  Duty cycle:  {stats['duty']:.1f}%")
        if expected:
            error_pct = abs(stats['frequency'] - expected) / expected * 100
            status = "OK" if error_pct < 10 else "MISMATCH"
            print(f"  Expected:    {expected/1000:.1f} kHz")
            print(f"  Error:       {error_pct:.1f}% [{status}]")

    # Check for common issues
    print("\n" + "-" * 60)
    print("Diagnostics:")

    bck_stats = analyze_signal(capture['bck'], 'BCK', sample_rate)
    ws_stats = analyze_signal(capture['ws'], 'WS', sample_rate)

    if bck_stats['frequency'] < 10000:
        print("  [ERROR] BCK signal appears dead or disconnected!")
        print(f"          Check wiring to GPIO {capture['pin_bck']}")
    elif bck_stats['frequency'] < 1000000:
        print("  [WARNING] BCK frequency too low - may be noise or wrong signal")

    if ws_stats['frequency'] < 1000:
        print("  [ERROR] WS signal appears dead or disconnected!")
        print(f"          Check wiring to GPIO {capture['pin_ws']}")
    elif ws_stats['frequency'] < 40000 or ws_stats['frequency'] > 70000:
        print("  [WARNING] WS frequency outside expected range (40-70 kHz)")

    # Check BCK/WS ratio
    if bck_stats['frequency'] > 1000 and ws_stats['frequency'] > 100:
        ratio = bck_stats['frequency'] / ws_stats['frequency']
        print(f"\n  BCK/WS ratio: {ratio:.1f} (expected: ~32)")
        if abs(ratio - 32) > 2:
            print("  [WARNING] BCK/WS ratio doesn't match expected 32 bits per sample")

def load_from_file(filename):
    """Load capture data from a binary file."""
    with open(filename, 'rb') as f:
        magic = f.read(4)
        if magic != b'LOGA':
            raise ValueError(f"Invalid file format (magic: {magic})")

        sample_rate = struct.unpack('<I', f.read(4))[0]
        num_samples = struct.unpack('<I', f.read(4))[0]
        pin_bck = f.read(1)[0]
        pin_dat = f.read(1)[0]
        pin_ws = f.read(1)[0]
        _ = f.read(1)

        data = f.read(num_samples)

        bck = np.array([(b & 0x01) for b in data], dtype=np.uint8)
        dat = np.array([(b & 0x02) >> 1 for b in data], dtype=np.uint8)
        ws = np.array([(b & 0x04) >> 2 for b in data], dtype=np.uint8)

        return {
            'sample_rate': sample_rate,
            'bck': bck,
            'dat': dat,
            'ws': ws,
            'pin_bck': pin_bck,
            'pin_dat': pin_dat,
            'pin_ws': pin_ws
        }

def save_to_file(capture, filename):
    """Save capture data to a binary file."""
    with open(filename, 'wb') as f:
        f.write(b'LOGA')
        f.write(struct.pack('<I', capture['sample_rate']))
        f.write(struct.pack('<I', len(capture['bck'])))
        f.write(bytes([capture['pin_bck'], capture['pin_dat'], capture['pin_ws'], 0]))

        # Pack data
        for i in range(len(capture['bck'])):
            b = capture['bck'][i] | (capture['dat'][i] << 1) | (capture['ws'][i] << 2)
            f.write(bytes([b]))

    print(f"Saved capture to {filename}")

def main():
    parser = argparse.ArgumentParser(description='YM2610 Audio Logic Analyzer Viewer')
    parser.add_argument('port', nargs='?', help='Serial port (auto-detect if not specified)')
    parser.add_argument('-f', '--file', help='Load from file instead of serial')
    parser.add_argument('-o', '--output', help='Save plot to file')
    parser.add_argument('-s', '--save', help='Save capture to binary file')
    parser.add_argument('-z', '--zoom', type=int, default=500, help='Zoom samples (default: 500)')
    parser.add_argument('--no-plot', action='store_true', help='Skip plotting')
    args = parser.parse_args()

    if args.file:
        # Load from file
        print(f"Loading from {args.file}...")
        capture = load_from_file(args.file)
    else:
        # Connect to Pico
        port = args.port or find_pico_port()
        if not port:
            print("Error: Could not find Pico serial port.")
            print("Please specify port manually, e.g.:")
            print("  python viewer.py /dev/tty.usbmodem12345")
            sys.exit(1)

        print(f"Connecting to {port}...")
        with serial.Serial(port, 115200, timeout=5) as ser:
            capture = capture_data(ser)

    if args.save:
        save_to_file(capture, args.save)

    print_analysis(capture)

    if not args.no_plot:
        plot_signals(capture, args.output, args.zoom)

if __name__ == '__main__':
    main()
