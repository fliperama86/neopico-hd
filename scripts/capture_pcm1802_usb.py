#!/usr/bin/env python3
"""Capture framed 16-bit or 24-bit PCM1802 audio from standalone firmware."""

import argparse
import dataclasses
import os
import secrets
import struct
import sys
import time
from types import SimpleNamespace
import wave
import zlib
from typing import Iterable, List, Optional


USB_VID = 0xCAFE
USB_PID = 0x4011
USB_PRODUCT = "NeoPico PCM1802 Capture"

PROTOCOL_VERSION = 1
COMMAND_MAGIC = b"NPCMCMD1"
STREAM_MAGIC = b"NPCMDAT1"

COMMAND_INFO = 1
COMMAND_START = 2
COMMAND_STOP = 3

PACKET_HELLO = 1
PACKET_STARTED = 2
PACKET_AUDIO = 3
PACKET_STOPPED = 4
PACKET_ERROR = 5

COMMAND_PREFIX = struct.Struct("<8sBBHI")
COMMAND_PACKET = struct.Struct("<8sBBHII")
STREAM_HEADER = struct.Struct("<8sBBHIIIQHBBIQ")
CRC32 = struct.Struct("<I")

SAMPLE_RATE = 48000
CHANNELS = 2
SUPPORTED_BITS_PER_SAMPLE = (16, 24)
TEST_BITS_PER_SAMPLE = 24
AUDIO_FRAMES_PER_PACKET = 128
AUDIO_PAYLOAD_BYTES = AUDIO_FRAMES_PER_PACKET * CHANNELS * (TEST_BITS_PER_SAMPLE // 8)

MAX_STREAM_PAYLOAD_BYTES = AUDIO_PAYLOAD_BYTES
HANDSHAKE_RETRY_SECONDS = 0.5
HANDSHAKE_TIMEOUT_SECONDS = 8.0
STOP_TIMEOUT_SECONDS = 1.0


@dataclasses.dataclass(frozen=True)
class StreamPacket:
    packet_type: int
    session_id: int
    sequence: int
    sample_rate: int
    first_frame: int
    frame_count: int
    channels: int
    bits_per_sample: int
    payload: bytes
    dropped_frames: int


class PacketDecoder:
    """Incrementally resynchronize and validate the CDC byte stream."""

    def __init__(self) -> None:
        self.buffer = bytearray()
        self.discarded_bytes = 0
        self.crc_errors = 0
        self.header_errors = 0

    def feed(self, data: bytes) -> List[StreamPacket]:
        self.buffer.extend(data)
        packets = []

        while True:
            magic_offset = self.buffer.find(STREAM_MAGIC)
            if magic_offset < 0:
                keep = min(len(self.buffer), len(STREAM_MAGIC) - 1)
                discarded = len(self.buffer) - keep
                if discarded:
                    del self.buffer[:discarded]
                    self.discarded_bytes += discarded
                break

            if magic_offset:
                del self.buffer[:magic_offset]
                self.discarded_bytes += magic_offset

            if len(self.buffer) < STREAM_HEADER.size:
                break

            fields = STREAM_HEADER.unpack_from(self.buffer)
            (
                magic,
                version,
                packet_type,
                header_bytes,
                session_id,
                sequence,
                sample_rate,
                first_frame,
                frame_count,
                channels,
                bits_per_sample,
                payload_bytes,
                dropped_frames,
            ) = fields

            if (
                magic != STREAM_MAGIC
                or version != PROTOCOL_VERSION
                or header_bytes != STREAM_HEADER.size
                or payload_bytes > MAX_STREAM_PAYLOAD_BYTES
            ):
                del self.buffer[0]
                self.header_errors += 1
                continue

            packet_bytes = header_bytes + payload_bytes + CRC32.size
            if len(self.buffer) < packet_bytes:
                break

            expected_crc = CRC32.unpack_from(self.buffer, header_bytes + payload_bytes)[0]
            actual_crc = zlib.crc32(self.buffer[: header_bytes + payload_bytes]) & 0xFFFFFFFF
            if actual_crc != expected_crc:
                del self.buffer[0]
                self.crc_errors += 1
                continue

            payload = bytes(self.buffer[header_bytes : header_bytes + payload_bytes])
            del self.buffer[:packet_bytes]
            packets.append(
                StreamPacket(
                    packet_type=packet_type,
                    session_id=session_id,
                    sequence=sequence,
                    sample_rate=sample_rate,
                    first_frame=first_frame,
                    frame_count=frame_count,
                    channels=channels,
                    bits_per_sample=bits_per_sample,
                    payload=payload,
                    dropped_frames=dropped_frames,
                )
            )

        return packets


class SampleStatistics:
    def __init__(self, bits_per_sample: int) -> None:
        self.bits_per_sample = bits_per_sample
        self.bytes_per_sample = bits_per_sample // 8
        positive_rail = (1 << (bits_per_sample - 1)) - 1
        negative_rail = -(1 << (bits_per_sample - 1))
        self.minimum = [positive_rail, positive_rail]
        self.maximum = [negative_rail, negative_rail]
        self.rail_samples = [0, 0]

    def add(self, payload: bytes) -> None:
        for offset in range(0, len(payload), self.bytes_per_sample):
            channel = (offset // self.bytes_per_sample) & 1
            value = payload[offset] | (payload[offset + 1] << 8)
            if self.bits_per_sample == 24:
                value |= payload[offset + 2] << 16
            sign_bit = 1 << (self.bits_per_sample - 1)
            if value & sign_bit:
                value -= 1 << self.bits_per_sample
            if value < self.minimum[channel]:
                self.minimum[channel] = value
            if value > self.maximum[channel]:
                self.maximum[channel] = value
            if value in (-(1 << (self.bits_per_sample - 1)), (1 << (self.bits_per_sample - 1)) - 1):
                self.rail_samples[channel] += 1

    def summary(self) -> str:
        return (
            f"L=[{self.minimum[0]}, {self.maximum[0]}], rails={self.rail_samples[0]}; "
            f"R=[{self.minimum[1]}, {self.maximum[1]}], rails={self.rail_samples[1]}"
        )


def make_command(command: int, session_id: int) -> bytes:
    prefix = COMMAND_PREFIX.pack(
        COMMAND_MAGIC,
        PROTOCOL_VERSION,
        command,
        COMMAND_PACKET.size,
        session_id,
    )
    return prefix + CRC32.pack(zlib.crc32(prefix) & 0xFFFFFFFF)


def send_command(serial_port, command: int, session_id: int) -> None:
    serial_port.write(make_command(command, session_id))
    serial_port.flush()


def read_packets(serial_port, decoder: PacketDecoder) -> Iterable[StreamPacket]:
    data = serial_port.read(max(1, serial_port.in_waiting))
    if not data:
        return []
    return decoder.feed(data)


def validate_device_parameters(packet: StreamPacket, expected_bits_per_sample: Optional[int] = None) -> None:
    if (
        packet.sample_rate != SAMPLE_RATE
        or packet.channels != CHANNELS
        or packet.bits_per_sample not in SUPPORTED_BITS_PER_SAMPLE
        or (
            expected_bits_per_sample is not None
            and packet.bits_per_sample != expected_bits_per_sample
        )
    ):
        raise RuntimeError(
            "Unsupported device format: "
            f"{packet.sample_rate} Hz, {packet.channels} channels, "
            f"{packet.bits_per_sample} bits"
        )


def wait_for_control_packet(
    serial_port,
    decoder: PacketDecoder,
    command: int,
    expected_type: int,
    session_id: int,
    timeout: float = HANDSHAKE_TIMEOUT_SECONDS,
) -> StreamPacket:
    deadline = time.monotonic() + timeout
    next_send = 0.0

    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_send:
            send_command(serial_port, command, session_id)
            next_send = now + HANDSHAKE_RETRY_SECONDS

        for packet in read_packets(serial_port, decoder):
            if packet.session_id != session_id:
                continue
            if packet.packet_type == PACKET_ERROR:
                raise RuntimeError("Device rejected the capture session")
            if packet.packet_type == expected_type:
                validate_device_parameters(packet)
                return packet

    raise TimeoutError(
        f"Timed out waiting for packet type {expected_type}. "
        "Check that the standalone capture UF2 is running and that no other program has the port open."
    )


def select_capture_port(ports: Iterable[object]) -> str:
    candidates = [
        port
        for port in ports
        if port.vid == USB_VID and port.pid == USB_PID
    ]
    if not candidates:
        raise RuntimeError(
            "NeoPico PCM1802 Capture was not found. Use --port if USB VID/PID detection is unavailable."
        )

    # CAFE:4011 is TinyUSB's example CDC-only VID/PID and is not unique to this
    # firmware. Prefer the product descriptor so another TinyUSB CDC device is
    # never selected merely because it is the only matching VID/PID device.
    matches = [
        port.device
        for port in candidates
        if port.product is not None and port.product.strip() == USB_PRODUCT
    ]
    if not matches and len(candidates) == 1 and candidates[0].product is None:
        # Some serial backends do not expose USB strings. The session handshake
        # still rejects non-capture firmware before an output WAV is opened.
        matches = [candidates[0].device]

    if not matches:
        descriptions = ", ".join(
            f"{port.device} ({port.product or 'USB product unavailable'})"
            for port in candidates
        )
        raise RuntimeError(
            f"CAFE:4011 device(s) were found, but none identified as {USB_PRODUCT!r}: {descriptions}. "
            "Select the capture device explicitly with --port if its USB product string is unavailable."
        )
    if len(matches) > 1:
        raise RuntimeError("More than one capture device was found. Select one with --port: " + ", ".join(matches))
    return matches[0]


def find_capture_port(explicit_port: Optional[str]) -> str:
    if explicit_port:
        return explicit_port

    try:
        from serial.tools import list_ports
    except ImportError as error:
        raise RuntimeError("pyserial is required: python3 -m pip install pyserial") from error

    return select_capture_port(list_ports.comports())


def write_silence(wav_file: wave.Wave_write, frame_count: int, bytes_per_frame: int) -> None:
    silence_frames = 4096
    silence = bytes(silence_frames * bytes_per_frame)
    while frame_count:
        count = min(frame_count, silence_frames)
        wav_file.writeframesraw(silence[: count * bytes_per_frame])
        frame_count -= count


def capture(args: argparse.Namespace) -> None:
    try:
        import serial
    except ImportError as error:
        raise RuntimeError("pyserial is required: python3 -m pip install pyserial") from error

    port = find_capture_port(args.port)
    session_id = secrets.randbits(32) or 1
    decoder = PacketDecoder()

    print(f"Opening {port}")
    with serial.Serial(port, 115200, timeout=0.050, write_timeout=2.0) as serial_port:
        serial_port.dtr = True
        serial_port.reset_input_buffer()
        serial_port.reset_output_buffer()

        print(f"Handshake session 0x{session_id:08x}: INFO -> HELLO")
        wait_for_control_packet(serial_port, decoder, COMMAND_INFO, PACKET_HELLO, session_id)
        print("Handshake: START -> STARTED")
        started = wait_for_control_packet(serial_port, decoder, COMMAND_START, PACKET_STARTED, session_id)

        bits_per_sample = started.bits_per_sample
        bytes_per_sample = bits_per_sample // 8
        bytes_per_frame = CHANNELS * bytes_per_sample

        expected_sequence = (started.sequence + 1) & 0xFFFFFFFF
        expected_frame = started.first_frame
        reported_drops = started.dropped_frames
        serial_packet_gaps = 0
        inserted_silence_frames = 0
        overlap_frames = 0
        audio_packets = 0
        stats = SampleStatistics(bits_per_sample)
        started_at = time.monotonic()
        last_audio_at = started_at
        last_status_at = started_at

        print(
            f"Capturing raw {SAMPLE_RATE} Hz, stereo, signed {bits_per_sample}-bit PCM to {args.output}. "
            "Press Ctrl-C to stop."
        )

        try:
            with wave.open(args.output, "wb") as wav_file:
                wav_file.setnchannels(CHANNELS)
                wav_file.setsampwidth(bytes_per_sample)
                wav_file.setframerate(SAMPLE_RATE)

                while args.seconds is None or time.monotonic() - started_at < args.seconds:
                    packets = read_packets(serial_port, decoder)
                    now = time.monotonic()

                    for packet in packets:
                        if packet.session_id != session_id:
                            continue
                        if packet.packet_type == PACKET_ERROR:
                            raise RuntimeError("Device reported a capture error")
                        if packet.packet_type != PACKET_AUDIO:
                            continue

                        validate_device_parameters(packet, bits_per_sample)
                        expected_payload = packet.frame_count * bytes_per_frame
                        if (
                            packet.frame_count == 0
                            or len(packet.payload) != expected_payload
                            or packet.frame_count > AUDIO_FRAMES_PER_PACKET
                        ):
                            raise RuntimeError("Device sent an invalid audio packet")

                        if packet.sequence != expected_sequence:
                            serial_packet_gaps += (packet.sequence - expected_sequence) & 0xFFFFFFFF
                        expected_sequence = (packet.sequence + 1) & 0xFFFFFFFF

                        payload = packet.payload
                        first_frame = packet.first_frame
                        frame_count = packet.frame_count

                        if first_frame > expected_frame:
                            gap = first_frame - expected_frame
                            write_silence(wav_file, gap, bytes_per_frame)
                            inserted_silence_frames += gap
                            expected_frame = first_frame
                        elif first_frame < expected_frame:
                            overlap = min(expected_frame - first_frame, frame_count)
                            overlap_frames += overlap
                            payload = payload[overlap * bytes_per_frame :]
                            first_frame += overlap
                            frame_count -= overlap

                        if frame_count:
                            wav_file.writeframesraw(payload)
                            stats.add(payload)
                            expected_frame = first_frame + frame_count

                        audio_packets += 1
                        last_audio_at = now
                        if packet.dropped_frames > reported_drops:
                            print(
                                f"Warning: device DMA dropped {packet.dropped_frames - reported_drops} "
                                f"additional source frames ({packet.dropped_frames} total)",
                                file=sys.stderr,
                            )
                            reported_drops = packet.dropped_frames

                    if now - last_audio_at > args.audio_timeout:
                        raise TimeoutError(
                            "No audio packets received. Check PCM1802 power, 12.288 MHz SCKI, "
                            "BCK/LRCK/DOUT wiring, and module mode straps."
                        )

                    if now - last_status_at >= 1.0:
                        elapsed = now - started_at
                        print(
                            f"{elapsed:7.1f}s, {expected_frame} source frames, "
                            f"device drops={reported_drops}, CRC errors={decoder.crc_errors}",
                            file=sys.stderr,
                        )
                        last_status_at = now
        except KeyboardInterrupt:
            print("Capture interrupted by user.")
        finally:
            try:
                send_command(serial_port, COMMAND_STOP, session_id)
                stop_deadline = time.monotonic() + STOP_TIMEOUT_SECONDS
                while time.monotonic() < stop_deadline:
                    if any(
                        packet.session_id == session_id and packet.packet_type == PACKET_STOPPED
                        for packet in read_packets(serial_port, decoder)
                    ):
                        break
            except (OSError, TimeoutError):
                pass

    duration = expected_frame / SAMPLE_RATE
    print(f"Saved {args.output}: {expected_frame} frames, {duration:.3f} s")
    print(
        "Integrity: "
        f"device drops={reported_drops}, serial packet gaps={serial_packet_gaps}, "
        f"CRC errors={decoder.crc_errors}, discarded bytes={decoder.discarded_bytes}, "
        f"inserted silence={inserted_silence_frames} frames, overlaps={overlap_frames} frames"
    )
    if audio_packets:
        print("Sample range: " + stats.summary())


def make_test_stream_packet(
    packet_type: int,
    session_id: int,
    sequence: int,
    first_frame: int,
    frame_count: int,
    payload: bytes,
    dropped_frames: int = 0,
    bits_per_sample: int = TEST_BITS_PER_SAMPLE,
) -> bytes:
    header = STREAM_HEADER.pack(
        STREAM_MAGIC,
        PROTOCOL_VERSION,
        packet_type,
        STREAM_HEADER.size,
        session_id,
        sequence,
        SAMPLE_RATE,
        first_frame,
        frame_count,
        CHANNELS,
        bits_per_sample,
        len(payload),
        dropped_frames,
    )
    packet = header + payload
    return packet + CRC32.pack(zlib.crc32(packet) & 0xFFFFFFFF)


def self_test() -> None:
    assert COMMAND_PACKET.size == 20
    assert STREAM_HEADER.size == 48

    command = make_command(COMMAND_START, 0x12345678)
    fields = COMMAND_PACKET.unpack(command)
    assert fields[:5] == (COMMAND_MAGIC, PROTOCOL_VERSION, COMMAND_START, 20, 0x12345678)
    assert fields[5] == (zlib.crc32(command[:-4]) & 0xFFFFFFFF)

    session_id = 0xA1B2C3D4
    hello = make_test_stream_packet(PACKET_HELLO, session_id, 0, 0, 0, b"")
    bad = bytearray(make_test_stream_packet(PACKET_AUDIO, session_id, 1, 0, 1, b"\x01\x02\x03\x04\x05\x06"))
    bad[-1] ^= 0x80
    audio_payload = bytes((index * 17) & 0xFF for index in range(AUDIO_PAYLOAD_BYTES))
    audio = make_test_stream_packet(
        PACKET_AUDIO,
        session_id,
        2,
        128,
        AUDIO_FRAMES_PER_PACKET,
        audio_payload,
        128,
    )

    stream = b"garbage-prefix" + hello + bytes(bad) + b"noise" + audio
    decoder = PacketDecoder()
    decoded = []
    chunk_sizes = [1, 2, 7, 3, 64, 5, 127, 11]
    offset = 0
    chunk_index = 0
    while offset < len(stream):
        size = chunk_sizes[chunk_index % len(chunk_sizes)]
        decoded.extend(decoder.feed(stream[offset : offset + size]))
        offset += size
        chunk_index += 1

    assert [packet.packet_type for packet in decoded] == [PACKET_HELLO, PACKET_AUDIO]
    assert decoded[1].payload == audio_payload
    assert decoded[1].first_frame == 128
    assert decoded[1].dropped_frames == 128
    assert decoder.crc_errors == 1
    assert decoder.discarded_bytes >= len(b"garbage-prefixnoise")

    audio16_payload = b"\x00\x80\xff\x7f\x34\x12\xcc\xed"
    audio16 = make_test_stream_packet(
        PACKET_AUDIO,
        session_id,
        3,
        256,
        2,
        audio16_payload,
        bits_per_sample=16,
    )
    decoded16 = PacketDecoder().feed(audio16)
    assert len(decoded16) == 1
    validate_device_parameters(decoded16[0], 16)
    stats16 = SampleStatistics(16)
    stats16.add(audio16_payload)
    assert stats16.minimum == [-32768, -4660]
    assert stats16.maximum == [4660, 32767]
    assert stats16.rail_samples == [1, 1]

    generic = SimpleNamespace(
        device="/dev/generic",
        vid=USB_VID,
        pid=USB_PID,
        product="TinyUSB Device",
    )
    capture_device = SimpleNamespace(
        device="/dev/capture",
        vid=USB_VID,
        pid=USB_PID,
        product=USB_PRODUCT,
    )
    assert select_capture_port([generic, capture_device]) == "/dev/capture"
    assert (
        select_capture_port(
            [SimpleNamespace(device="/dev/no-strings", vid=USB_VID, pid=USB_PID, product=None)]
        )
        == "/dev/no-strings"
    )
    try:
        select_capture_port([generic])
    except RuntimeError as error:
        assert "none identified" in str(error)
    else:
        raise AssertionError("Generic TinyUSB CDC device was accepted as the capture firmware")

    print("Protocol self-test passed")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture raw PCM1802 DOUT from the standalone NeoPico USB firmware into a WAV file."
    )
    parser.add_argument("output", nargs="?", help="Output 48 kHz stereo WAV path")
    parser.add_argument("--port", help="Serial port. By default the capture firmware is auto-detected.")
    parser.add_argument("--seconds", type=float, help="Stop automatically after this many seconds")
    parser.add_argument(
        "--audio-timeout",
        type=float,
        default=3.0,
        help="Abort after this many seconds without an audio packet (default: 3)",
    )
    parser.add_argument("--self-test", action="store_true", help="Test framing and recovery without hardware")
    args = parser.parse_args()

    if args.self_test:
        return args
    if not args.output:
        parser.error("the output WAV path is required unless --self-test is used")
    if args.seconds is not None and args.seconds <= 0:
        parser.error("--seconds must be positive")
    if args.audio_timeout <= 0:
        parser.error("--audio-timeout must be positive")
    return args


def main() -> int:
    args = parse_args()
    if args.self_test:
        self_test()
        return 0

    try:
        capture(args)
    except (OSError, RuntimeError, TimeoutError) as error:
        print(f"error: {error}", file=sys.stderr)
        if args.output and os.path.exists(args.output) and os.path.getsize(args.output) <= 44:
            os.unlink(args.output)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
