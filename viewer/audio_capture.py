#!/usr/bin/env python3
"""
Simple audio capture from Pico to WAV
Usage: python3 audio_capture.py [port] [seconds] [output.wav]
"""

import sys
import wave
import serial
import time
import glob

SAMPLE_RATE = 55500
CHANNELS = 2
SAMPLE_WIDTH = 2

def find_port():
    ports = glob.glob('/dev/tty.usbmodem*') + glob.glob('/dev/ttyACM*')
    return ports[0] if ports else None

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 20
    output = sys.argv[3] if len(sys.argv) > 3 else f"capture_{int(time.time())}.wav"

    if not port:
        print("No serial port found")
        sys.exit(1)

    bytes_needed = int(SAMPLE_RATE * duration * CHANNELS * SAMPLE_WIDTH)

    print(f"Port: {port}")
    print(f"Duration: {duration}s ({bytes_needed} bytes)")
    print(f"Output: {output}")

    ser = serial.Serial(port, 115200, timeout=2)
    time.sleep(0.3)
    ser.reset_input_buffer()

    # Wait for READY
    print("Waiting for device...")
    while True:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
        if line:
            print(f"  {line}")
        if 'READY' in line:
            break

    # Trigger start
    print("Starting capture...")
    ser.write(b'x')

    # Wait for STREAM_START
    while True:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
        if line:
            print(f"  {line}")
        if 'STREAM_START' in line:
            break

    # Capture audio
    audio = bytearray()
    start = time.time()

    while len(audio) < bytes_needed:
        chunk = ser.read(min(8192, bytes_needed - len(audio)))
        if chunk:
            audio.extend(chunk)
        elapsed = time.time() - start
        pct = len(audio) / bytes_needed * 100
        print(f"\r  {len(audio)}/{bytes_needed} ({pct:.1f}%) - {elapsed:.1f}s", end='', flush=True)

    print(f"\n\nCaptured {len(audio)} bytes")
    ser.close()

    # Save WAV
    with wave.open(output, 'wb') as w:
        w.setnchannels(CHANNELS)
        w.setsampwidth(SAMPLE_WIDTH)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(bytes(audio))

    print(f"Saved: {output}")

if __name__ == '__main__':
    main()
