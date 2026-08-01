#!/usr/bin/env python3
"""Read and summarise the NeoPico-HD scanline timing trace.

The firmware (NEOPICO_EXP_SCANLINE_TRACE=ON) records how many CPU cycles each
Core 1 scanline callback takes, into a RAM ring buffer. Core 0 dumps that ring
as raw binary when this script pokes the CDC port. Nothing is formatted on the
device: snprintf on Core 1 is a documented cause of HSTX FIFO underruns in this
firmware, so the decode happens here instead.

Trigger the dump AFTER observing the failure. By then the output is already
broken, so the USB traffic cannot be blamed for what was captured.

Usage:
    python3 scripts/read_scanline_trace.py [--port /dev/cu.usbmodemXXXX]
                                           [--sysclk-mhz 252] [--raw out.bin]
"""

import argparse
import glob
import struct
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover
    sys.exit("pyserial is required: pip3 install pyserial")

MAGIC = 0x5254504E  # 'NPTR' little-endian
HEADER = struct.Struct("<III")


def find_port():
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyACM*"):
        ports = sorted(glob.glob(pattern))
        if ports:
            return ports[0]
    return None


def read_exact(port, count, timeout_s=5.0):
    """Read exactly `count` bytes, giving up after `timeout_s` of silence.

    pyserial's per-call timeout returns short reads rather than blocking, so
    the deadline here is a silence timeout: it resets whenever bytes arrive.
    """
    data = bytearray()
    deadline = time.time() + timeout_s
    while len(data) < count and time.time() < deadline:
        chunk = port.read(count - len(data))
        if chunk:
            data.extend(chunk)
            deadline = time.time() + timeout_s
    return bytes(data)


def percentile(values, fraction):
    if not values:
        return 0
    ordered = sorted(values)
    idx = min(len(ordered) - 1, int(fraction * len(ordered)))
    return ordered[idx]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="CDC device (default: first usbmodem/ttyACM)")
    ap.add_argument("--sysclk-mhz", type=float, default=252.0,
                    help="system clock, for converting cycles to microseconds")
    ap.add_argument("--raw", help="also write the undecoded ring to this file")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("No CDC port found; pass --port explicitly.")

    with serial.Serial(port, 115200, timeout=0.100, write_timeout=2.0) as sp:
        sp.reset_input_buffer()
        sp.write(b"\n")  # any byte requests a dump
        sp.flush()
        header = read_exact(sp, HEADER.size)
        if len(header) < HEADER.size:
            sys.exit(
                "No header received.\n"
                "  - Is this build NEOPICO_EXP_SCANLINE_TRACE=ON? (build-trace, not build-e2e)\n"
                "  - Is the firmware running (not in BOOTSEL)?"
            )
        magic, entries, write_idx = HEADER.unpack(header)
        if magic != MAGIC:
            sys.exit(f"Bad magic 0x{magic:08X}; expected 0x{MAGIC:08X}.")

        payload = read_exact(sp, entries * 2)

    if len(payload) < entries * 2:
        sys.exit(f"Short read: {len(payload)} of {entries * 2} bytes.")
    if args.raw:
        with open(args.raw, "wb") as out:
            out.write(payload)

    samples = list(struct.unpack(f"<{entries}H", payload))

    # The ring is written modulo `entries`, so the oldest sample sits at the
    # write index. Reorder into chronological order, newest last.
    head = write_idx % entries
    ordered = samples[head:] + samples[:head]
    total_lines = min(write_idx, entries)
    ordered = ordered[len(ordered) - total_lines:] if total_lines else []

    active = [v for v in ordered if v > 0]
    cyc_per_us = args.sysclk_mhz

    print(f"port            : {port}")
    print(f"lines captured  : {total_lines} (ring holds {entries})")
    print(f"lines with work : {len(active)}  ({len(ordered) - len(active)} near-zero, e.g. 3x skipped lines)")
    if not active:
        print("No non-zero samples: the callback never ran, or the trace never armed.")
        return

    print()
    print(f"{'':16}{'cycles':>10}{'us':>10}")
    for label, value in (
        ("min", min(active)),
        ("median", percentile(active, 0.50)),
        ("p90", percentile(active, 0.90)),
        ("p99", percentile(active, 0.99)),
        ("max", max(active)),
    ):
        print(f"{label:16}{value:>10}{value / cyc_per_us:>10.2f}")

    saturated = sum(1 for v in ordered if v == 0xFFFF)
    if saturated:
        print(f"\nWARNING: {saturated} samples saturated at 65535 cycles (true duration is higher).")

    print("\nlast 32 lines before the dump (chronological, cycles):")
    tail = ordered[-32:]
    for i in range(0, len(tail), 16):
        print("  " + " ".join(f"{v:5d}" for v in tail[i:i + 16]))


if __name__ == "__main__":
    main()
