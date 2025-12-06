#!/usr/bin/env python3
"""
MVS I2S Digital Audio Receiver

Receives stereo digital audio from RP2350 via USB and plays it through speakers.
This is for testing I2S capture - the audio is already digital, no ADC conversion needed.

Usage:
    python i2s_audio_test.py [--port /dev/tty.usbmodem*] [--no-play] [--save output.wav]

Requirements:
    pip install pyserial pyaudio numpy

On macOS you may need:
    brew install portaudio
    pip install pyaudio
"""

import argparse
import struct
import sys
import time
import threading
import queue
from collections import deque

import serial
import serial.tools.list_ports

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False
    print("Warning: numpy not installed, visualization disabled")

try:
    import pyaudio
    HAS_PYAUDIO = True
except ImportError:
    HAS_PYAUDIO = False
    print("Warning: pyaudio not installed, audio playback disabled")

# Protocol constants (must match Pico)
SYNC_HEADER = bytes([0xDE, 0xAD, 0xBE, 0xEF])

# MVS YM2610 sample rate is ~55.5 kHz
# We'll play back at 48kHz (standard) - slight pitch adjustment
MVS_SAMPLE_RATE = 55555
PLAYBACK_SAMPLE_RATE = 48000
SAMPLES_PER_BUFFER = 128


def find_pico_port():
    """Auto-detect Pico CDC port."""
    ports = serial.tools.list_ports.comports()
    for port in ports:
        if 'ACM' in port.device or 'usbmodem' in port.device:
            return port.device
        if port.vid == 0x2E8A:  # Raspberry Pi vendor ID
            return port.device
    return None


def sync_to_packet(ser):
    """Find sync header in stream."""
    buffer = bytearray(4)
    bytes_searched = 0
    max_bytes = 10000

    while bytes_searched < max_bytes:
        byte = ser.read(1)
        if not byte:
            continue
        buffer.pop(0)
        buffer.append(byte[0])
        bytes_searched += 1
        if bytes(buffer) == SYNC_HEADER:
            return True
    return False


def read_packet(ser):
    """Read a complete audio packet (stereo 16-bit samples)."""
    # Read sample count (2 bytes, little-endian) - this is STEREO sample count
    count_bytes = ser.read(2)
    if len(count_bytes) != 2:
        return None

    stereo_sample_count = struct.unpack('<H', count_bytes)[0]

    # Read stereo samples (interleaved L/R, 16-bit each = 4 bytes per stereo sample)
    data_size = stereo_sample_count * 4  # 2 channels * 2 bytes per sample
    sample_data = ser.read(data_size)
    if len(sample_data) != data_size:
        return None

    return stereo_sample_count, sample_data


def convert_to_audio(sample_data, stereo_sample_count):
    """Convert I2S samples to playback format.

    I2S data is already 16-bit signed stereo - just pass through!
    Optionally resample from 55.5kHz to 48kHz for better compatibility.
    """
    if stereo_sample_count == 0:
        return b''

    # Data is already interleaved 16-bit signed stereo
    # Just return as-is for now (slight pitch shift is acceptable for testing)
    return sample_data


class AudioPlayer:
    """Threaded audio playback using PyAudio."""

    def __init__(self, sample_rate=48000, channels=2):
        self.sample_rate = sample_rate
        self.channels = channels
        self.audio_queue = queue.Queue(maxsize=30)
        self.running = False
        self.thread = None

        if HAS_PYAUDIO:
            self.pa = pyaudio.PyAudio()
            self.stream = self.pa.open(
                format=pyaudio.paInt16,
                channels=channels,
                rate=sample_rate,
                output=True,
                frames_per_buffer=SAMPLES_PER_BUFFER
            )
        else:
            self.pa = None
            self.stream = None

    def start(self):
        if not HAS_PYAUDIO:
            return
        self.running = True
        self.thread = threading.Thread(target=self._playback_thread, daemon=True)
        self.thread.start()

    def stop(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=1.0)
        if self.stream:
            self.stream.stop_stream()
            self.stream.close()
        if self.pa:
            self.pa.terminate()

    def queue_audio(self, audio_data):
        if not HAS_PYAUDIO:
            return
        try:
            self.audio_queue.put_nowait(audio_data)
        except queue.Full:
            # Drop oldest to prevent latency buildup
            try:
                self.audio_queue.get_nowait()
                self.audio_queue.put_nowait(audio_data)
            except queue.Empty:
                pass

    def _playback_thread(self):
        while self.running:
            try:
                audio_data = self.audio_queue.get(timeout=0.1)
                self.stream.write(audio_data)
            except queue.Empty:
                continue


class StereoWaveformDisplay:
    """Simple terminal-based stereo waveform display."""

    def __init__(self, width=50):
        self.width = width
        self.left_level = 0.0
        self.right_level = 0.0

    def update(self, sample_data, stereo_sample_count):
        if not HAS_NUMPY or stereo_sample_count == 0:
            return

        # Unpack as interleaved 16-bit signed stereo
        samples = np.frombuffer(sample_data, dtype=np.int16)

        if len(samples) < 2:
            return

        # Deinterleave
        left = samples[0::2].astype(np.float32)
        right = samples[1::2].astype(np.float32)

        # Calculate RMS levels (normalized to 0-1)
        self.left_level = min(1.0, np.sqrt(np.mean(left ** 2)) / 16384)
        self.right_level = min(1.0, np.sqrt(np.mean(right ** 2)) / 16384)

    def render(self):
        left_bar = self._render_bar(self.left_level)
        right_bar = self._render_bar(self.right_level)
        return f"L: {left_bar}\nR: {right_bar}"

    def _render_bar(self, level):
        bar_width = int(level * (self.width - 3))
        bar = "#" * bar_width + "-" * (self.width - 3 - bar_width)
        return f"[{bar}]"


def main():
    parser = argparse.ArgumentParser(description='MVS I2S Digital Audio Receiver')
    parser.add_argument('--port', '-p', help='Serial port (auto-detect if not specified)')
    parser.add_argument('--no-play', action='store_true', help='Disable audio playback')
    parser.add_argument('--save', '-s', metavar='FILE', help='Save audio to WAV file')
    parser.add_argument('--baud', '-b', type=int, default=115200, help='Baud rate')
    parser.add_argument('--quiet', '-q', action='store_true', help='Minimal output')
    parser.add_argument('--sample-rate', '-r', type=int, default=MVS_SAMPLE_RATE,
                        help=f'Playback sample rate (default: {MVS_SAMPLE_RATE})')
    args = parser.parse_args()

    # Find port
    port = args.port or find_pico_port()
    if not port:
        print("Error: Could not find Pico. Specify port with --port")
        print("Available ports:")
        for p in serial.tools.list_ports.comports():
            print(f"  {p.device}: {p.description}")
        sys.exit(1)

    print(f"Connecting to {port}...")
    print(f"Playback sample rate: {args.sample_rate} Hz")

    try:
        ser = serial.Serial(port, args.baud, timeout=0.5)
        ser.reset_input_buffer()
    except serial.SerialException as e:
        print(f"Error opening port: {e}")
        sys.exit(1)

    # Setup audio playback
    player = None
    if not args.no_play and HAS_PYAUDIO:
        print("Starting stereo audio playback...")
        player = AudioPlayer(args.sample_rate, 2)  # Stereo
        player.start()

    # Setup WAV file saving
    wav_samples = []
    if args.save:
        print(f"Recording to {args.save}...")

    # Setup waveform display
    waveform = StereoWaveformDisplay()

    print("\nWaiting for I2S audio data... (Ctrl+C to stop)\n")
    print("")  # Extra lines for waveform display
    print("")

    packet_count = 0
    start_time = time.time()
    last_display_time = start_time
    resync_count = 0

    try:
        while True:
            # Sync to packet
            if not sync_to_packet(ser):
                print("\rLost sync, searching...     ", end='')
                resync_count += 1
                continue

            # Read packet
            result = read_packet(ser)
            if result is None:
                resync_count += 1
                continue

            stereo_sample_count, sample_data = result
            packet_count += 1

            # Convert and play audio
            audio_data = convert_to_audio(sample_data, stereo_sample_count)

            if player:
                player.queue_audio(audio_data)

            if args.save:
                wav_samples.append(audio_data)

            # Update waveform display
            waveform.update(sample_data, stereo_sample_count)

            # Display status every ~100ms
            now = time.time()
            if not args.quiet and now - last_display_time >= 0.1:
                elapsed = now - start_time
                rate = packet_count / elapsed if elapsed > 0 else 0
                effective_rate = rate * SAMPLES_PER_BUFFER / 1000

                # Move up 3 lines and redraw
                sys.stdout.write("\033[3A\033[J")
                print(f"Packets: {packet_count:6d} | Rate: {rate:5.1f} pkt/s ({effective_rate:.1f} kHz) | Resyncs: {resync_count}")
                print(waveform.render())
                sys.stdout.flush()

                last_display_time = now

    except KeyboardInterrupt:
        print("\n\nStopping...")

    finally:
        ser.close()

        if player:
            player.stop()

        # Save WAV file
        if args.save and wav_samples:
            import wave
            print(f"Saving {len(wav_samples)} packets to {args.save}...")
            with wave.open(args.save, 'wb') as wf:
                wf.setnchannels(2)  # Stereo
                wf.setsampwidth(2)  # 16-bit
                wf.setframerate(args.sample_rate)
                wf.writeframes(b''.join(wav_samples))
            print(f"Saved {args.save}")

    print("Done.")


if __name__ == '__main__':
    main()
