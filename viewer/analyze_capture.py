#!/usr/bin/env python3
"""
YM2610 Audio Capture Analyzer

Captures audio data from Pico and analyzes the format.
Automatically detects serial port and processes data.

Usage: python3 analyze_capture.py [--port /dev/tty.usbmodem*]
"""

import sys
import struct
import serial
import serial.tools.list_ports
import numpy as np
from pathlib import Path
import time

def find_pico_port():
    """Find the Pico's serial port."""
    ports = list(serial.tools.list_ports.comports())
    for port in ports:
        if 'usbmodem' in port.device.lower() or 'acm' in port.device.lower():
            return port.device
    return None

def decode_ym2610_cps2(raw, bit_offset=0):
    """Decode using cps2_digiav algorithm: (mant-512) << (exp-1)

    bit_offset: how many bits to shift right before extracting (for testing different alignments)
    """
    raw = int(raw) >> bit_offset
    exp = (raw >> 13) & 0x7
    mant = (raw >> 3) & 0x3FF
    value = int(mant) - 512  # signed
    if exp > 0:
        value <<= (exp - 1)
    return value

def decode_ym2610_16bit_signed(raw, bit_offset=0):
    """Simple 16-bit signed interpretation"""
    raw = int(raw) >> bit_offset
    val = raw & 0xFFFF
    if val >= 0x8000:
        val = val - 0x10000
    return val

def decode_ym2610_simple(raw):
    """Simple decode: just treat as 16-bit PCM."""
    return np.int16(raw)

def analyze_data(left, right):
    """Analyze captured data with 32-bit values."""
    print("\n" + "="*60)
    print("DATA ANALYSIS")
    print("="*60)

    # Basic stats
    print(f"\nSamples: {len(left)}")
    print(f"Left:  min=0x{left.min():08X} max=0x{left.max():08X}")
    print(f"Right: min=0x{right.min():08X} max=0x{right.max():08X}")

    # Check for stuck/constant values
    unique_l = len(np.unique(left))
    unique_r = len(np.unique(right))
    print(f"\nUnique values: L={unique_l}, R={unique_r}")

    # Find the actual bit width used
    max_val = max(left.max(), right.max())
    if max_val > 0:
        actual_bits = int(np.ceil(np.log2(max_val + 1)))
    else:
        actual_bits = 0
    print(f"Actual bit width: {actual_bits} bits (max value needs {actual_bits} bits)")

    # Try different bit alignments for cps2_digiav decode
    print("\n--- Testing different bit offsets for cps2_digiav decode ---")
    best_offset = 0
    best_std = 0

    for offset in range(8):  # Try offsets 0-7
        # Decode using cps2_digiav algorithm at this offset
        decoded_l = np.array([decode_ym2610_cps2(x, offset) for x in left[:1000]])
        decoded_r = np.array([decode_ym2610_cps2(x, offset) for x in right[:1000]])

        l_std = np.std(decoded_l)
        r_std = np.std(decoded_r)
        avg_std = (l_std + r_std) / 2

        l_mean = np.mean(decoded_l)
        r_mean = np.mean(decoded_r)

        # Extract exponents at this offset
        exp_l = (left[:1000] >> (offset + 13)) & 0x7
        exp_range = f"[{exp_l.min()},{exp_l.max()}]"

        print(f"  offset={offset}: exp_range={exp_range:8s} L_std={l_std:8.1f} R_std={r_std:8.1f} "
              f"L_mean={l_mean:8.1f} R_mean={r_mean:8.1f}")

        if avg_std > best_std:
            best_std = avg_std
            best_offset = offset

    print(f"\nBest offset (highest variance): {best_offset}")

    # Full decode with best offset
    decoded_l = np.array([decode_ym2610_cps2(x, best_offset) for x in left])
    decoded_r = np.array([decode_ym2610_cps2(x, best_offset) for x in right])

    # Also try raw 16-bit signed at different offsets
    print("\n--- Testing 16-bit signed decode at different offsets ---")
    for offset in range(8):
        raw_l = np.array([decode_ym2610_16bit_signed(x, offset) for x in left[:1000]])
        raw_r = np.array([decode_ym2610_16bit_signed(x, offset) for x in right[:1000]])
        print(f"  offset={offset}: L_range=[{raw_l.min():6d},{raw_l.max():6d}] "
              f"R_range=[{raw_r.min():6d},{raw_r.max():6d}] "
              f"L_std={np.std(raw_l):8.1f}")

    # Exponent distribution at best offset
    exp_l = (left >> (best_offset + 13)) & 0x7
    exp_r = (right >> (best_offset + 13)) & 0x7

    print(f"\nExponent distribution at offset {best_offset} (bits[{best_offset+15}:{best_offset+13}]):")
    for e in range(8):
        count_l = np.sum(exp_l == e)
        count_r = np.sum(exp_r == e)
        if count_l > 0 or count_r > 0:
            print(f"  exp={e}: L={count_l:6d} ({100*count_l/len(left):5.1f}%)  "
                  f"R={count_r:6d} ({100*count_r/len(right):5.1f}%)")

    # Find samples with actual audio (non-silence)
    threshold = 100
    active_l = np.sum(np.abs(decoded_l) > threshold)
    active_r = np.sum(np.abs(decoded_r) > threshold)
    print(f"\nActive samples (|value|>{threshold}): L={active_l} ({100*active_l/len(left):.1f}%), "
          f"R={active_r} ({100*active_r/len(right):.1f}%)")

    return decoded_l, decoded_r, best_offset

def save_wav(filename, left, right, sample_rate=55500):
    """Save decoded audio as WAV file."""
    import wave

    # Scale to 16-bit range
    max_val = max(np.abs(left).max(), np.abs(right).max(), 1)
    scale = 32767 / max_val if max_val > 0 else 1

    left_16 = np.clip(left * scale, -32768, 32767).astype(np.int16)
    right_16 = np.clip(right * scale, -32768, 32767).astype(np.int16)

    # Interleave stereo
    stereo = np.empty(len(left) * 2, dtype=np.int16)
    stereo[0::2] = left_16
    stereo[1::2] = right_16

    with wave.open(filename, 'w') as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(int(sample_rate))
        wav.writeframes(stereo.tobytes())

    print(f"\nSaved: {filename}")

def main():
    port = None
    if len(sys.argv) > 2 and sys.argv[1] == '--port':
        port = sys.argv[2]

    if not port:
        port = find_pico_port()

    if not port:
        print("Error: No Pico found. Connect Pico or specify --port")
        sys.exit(1)

    print(f"Connecting to {port}...")

    try:
        ser = serial.Serial(port, 115200, timeout=5)
    except Exception as e:
        print(f"Error opening port: {e}")
        sys.exit(1)

    # Read all header info with timeout (READY, FREQ, BITS, RATE, DATA)
    print("Waiting for Pico...")
    freq_info = {}
    bits_info = {}
    sample_rate = 55500
    num_samples = 0
    bytes_per_sample = 8
    got_ready = False

    header_start = time.time()
    while time.time() - header_start < 30:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
        if not line:
            continue

        print(f"< {line}")

        if line == "READY":
            got_ready = True
        elif line.startswith("FREQ:"):
            try:
                parts = line[5:].split(',')
                for p in parts:
                    k, v = p.split('=')
                    freq_info[k] = float(v)
            except:
                pass
        elif line.startswith("BITS:"):
            try:
                parts = line[5:].split(',')
                for p in parts:
                    k, v = p.split('=')
                    bits_info[k] = int(v)
            except:
                pass
        elif line.startswith("RATE:"):
            try:
                sample_rate = float(line[5:])
            except:
                pass
        elif line.startswith("DATA32:"):
            num_samples = int(line[7:])
            bytes_per_sample = 8  # 32-bit L + 32-bit R
            break
        elif line.startswith("DATA:"):
            num_samples = int(line[5:])
            bytes_per_sample = 4  # 16-bit L + 16-bit R
            break

    if num_samples == 0:
        print("ERROR: Timeout waiting for DATA header.")
        ser.close()
        sys.exit(1)

    print(f"\nCapturing {num_samples} samples ({bytes_per_sample} bytes each)...")

    # Read binary data with timeout
    bytes_needed = num_samples * bytes_per_sample
    data = b''
    data_start = time.time()
    ser.timeout = 2  # Short timeout for individual reads

    while len(data) < bytes_needed:
        if time.time() - data_start > 30:
            print(f"\nWarning: Timeout after {len(data)}/{bytes_needed} bytes")
            break
        chunk = ser.read(min(4096, bytes_needed - len(data)))
        if not chunk:
            print(f"\nWarning: Read returned empty, got {len(data)}/{bytes_needed} bytes")
            break
        data += chunk
        pct = 100 * len(data) / bytes_needed
        print(f"\r  {len(data)}/{bytes_needed} bytes ({pct:.0f}%)", end='', flush=True)

    print()

    # Wait for DONE (brief timeout)
    ser.timeout = 1
    line = ser.readline().decode('utf-8', errors='ignore').strip()
    if line:
        print(f"< {line}")

    ser.close()

    # Parse binary data
    if bytes_per_sample == 8:
        # 32-bit data
        if len(data) < bytes_needed:
            num_samples = len(data) // 8
        left = np.zeros(num_samples, dtype=np.uint32)
        right = np.zeros(num_samples, dtype=np.uint32)
        for i in range(num_samples):
            offset = i * 8
            if offset + 8 <= len(data):
                left[i] = struct.unpack('<I', data[offset:offset+4])[0]
                right[i] = struct.unpack('<I', data[offset+4:offset+8])[0]
    else:
        # 16-bit data (legacy)
        if len(data) < bytes_needed:
            num_samples = len(data) // 4
        left = np.zeros(num_samples, dtype=np.uint32)
        right = np.zeros(num_samples, dtype=np.uint32)
        for i in range(num_samples):
            offset = i * 4
            if offset + 4 <= len(data):
                left[i] = struct.unpack('<H', data[offset:offset+2])[0]
                right[i] = struct.unpack('<H', data[offset+2:offset+4])[0]

    # Save raw data
    raw_file = Path("capture_raw.bin")
    raw_file.write_bytes(data)
    print(f"Saved raw data: {raw_file}")

    # Analyze
    decoded_l, decoded_r, best_offset = analyze_data(left, right)

    # Save as WAV
    save_wav("capture_decoded.wav", decoded_l, decoded_r, sample_rate)

    # Also save raw 16-bit at different offsets for comparison
    for offset in [0, 4]:
        raw_l = np.array([decode_ym2610_16bit_signed(x, offset) for x in left])
        raw_r = np.array([decode_ym2610_16bit_signed(x, offset) for x in right])
        save_wav(f"capture_raw16_offset{offset}.wav", raw_l, raw_r, sample_rate)

    # Save decoded at best offset
    if best_offset != 0:
        save_wav(f"capture_decoded_offset{best_offset}.wav", decoded_l, decoded_r, sample_rate)

    # Print first 20 samples for inspection
    print("\n" + "="*60)
    print("FIRST 20 SAMPLES (32-bit raw values)")
    print("="*60)
    print("idx       L_raw          R_raw      L_dec   R_dec")
    for i in range(min(20, len(left))):
        l, r = left[i], right[i]
        l_dec = decode_ym2610_cps2(l, best_offset)
        r_dec = decode_ym2610_cps2(r, best_offset)
        print(f"{i:3d}  0x{l:08X}   0x{r:08X}   {l_dec:7d} {r_dec:7d}")

    # Also print binary representation of first 5 samples
    print("\n--- Binary representation of first 5 samples ---")
    for i in range(min(5, len(left))):
        l, r = left[i], right[i]
        # Show as 24-bit binary (most likely bit width)
        l_bin = f"{l:024b}" if l < 0x1000000 else f"{l:032b}"
        r_bin = f"{r:024b}" if r < 0x1000000 else f"{r:032b}"
        print(f"{i}: L={l_bin}  R={r_bin}")

    print("\n" + "="*60)
    print("FILES CREATED:")
    print("  capture_raw.bin              - Raw binary data")
    print("  capture_decoded.wav          - Decoded using cps2_digiav algorithm")
    print("  capture_raw16_offset0.wav    - Raw 16-bit at offset 0")
    print("  capture_raw16_offset4.wav    - Raw 16-bit at offset 4")
    if best_offset != 0:
        print(f"  capture_decoded_offset{best_offset}.wav  - Decoded at best offset")
    print("="*60)
    print("\nCompare these WAV files with viewer/intro.flac to find correct decode!")

if __name__ == "__main__":
    main()
