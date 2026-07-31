# Standalone PCM1802 USB Capture

This diagnostic firmware captures the PCM1802 serial output directly as signed
24-bit stereo PCM. Its default USB transport preserves all 24 bits; an optional
reduced-bandwidth build sends the upper 16 bits without changing PCM1802, PIO,
or DMA capture. It does not initialize Neo Geo video capture, HSTX, HDMI audio,
sample-rate conversion, volume, EQ, or filters. It is intended to answer one
question: is the PCM1802 output already distorted before the normal NeoPico-HD
audio pipeline receives it?

The 24-bit firmware completed a local 12.288 MHz hardware capture at 48 kHz with
zero DMA drops, packet gaps, CRC errors, or inserted silence. A separate Windows
host plateaued near 255.5 kB/s and overran the DMA ring at the full 24-bit rate;
the 16-bit transport is an isolation test for that host-specific bottleneck.

Before connecting USB, prevent backfeeding between USB 5 V and external 5 V.
Disconnect external power unless the installation has verified power-path
isolation. Keep a solid shared ground between the PCM1802 and RP2350B, and do
not alter wiring while either board is powered.

## Hardware format and pins

The diagnostic uses the same PCM1802 connection as the production firmware:

| Signal | RP2350B pin |
| --- | --- |
| PCM1802 DOUT | GP22 |
| PCM1802 LRCK | GP23 |
| PCM1802 BCK | GP24 |

The expected PCM1802 configuration is standard Philips I2S, 24-bit, master
mode at 256fs:

- FMT1 = GND
- FMT0 = VDD
- MODE1 = VDD
- MODE0 = VDD
- SCKI = 12.288 MHz
- LRCK/fs = 48 kHz
- BCK = 3.072 MHz, or 64fs
- LRCK low = left, LRCK high = right

## Build

From the repository root:

```sh
cmake -S . -B build-pcm1802-usb \
  -DPICO_SDK_PATH="$HOME/pico-sdk" \
  -DNEOPICO_BUILD_PCM1802_USB_CAPTURE=ON
cmake --build build-pcm1802-usb --target neopico_pcm1802_usb_capture -j
```

The standalone UF2 is:

```text
build-pcm1802-usb/src/neopico_pcm1802_usb_capture.uf2
```

This is a separate, default-off target. Building it does not change the normal
`neopico_hd` firmware.

The default payload preserves all 24 captured bits. Build the reduced-bandwidth
16-bit transport in a separate directory with:

```sh
cmake -S . -B build-pcm1802-usb-16 \
  -DPICO_SDK_PATH="$HOME/pico-sdk" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOPICO_BUILD_PCM1802_USB_CAPTURE=ON \
  -DNEOPICO_PCM1802_USB_BITS_PER_SAMPLE=16
cmake --build build-pcm1802-usb-16 --target neopico_pcm1802_usb_capture -j
```

At 48 kHz, the framed 16-bit stream requires 211,500 bytes/s instead of
307,500 bytes/s. Capture remains 24-bit internally; only the USB packet staging
drops the least-significant byte from each sample.

Create the self-contained share bundle after building:

```sh
python3 scripts/package_pcm1802_usb_capture.py
```

The generated ZIP uses the package-specific instructions in
`docs/PCM1802_USB_CAPTURE_BUNDLE.md`, includes checksums, and does not require a
repository checkout.

Create a separate source-only ZIP, with a standalone CMake project and no
checksum or firmware binary files:

```sh
python3 scripts/package_pcm1802_usb_capture_source.py
```

## Host setup and capture

Install the one host dependency:

```sh
python3 -m pip install pyserial
```

Start a capture at any time after the device enumerates:

```sh
python3 scripts/capture_pcm1802_usb.py pcm1802-raw.wav
```

The script auto-detects the capture firmware by USB VID/PID and checks its USB
product descriptor when the operating system exposes it. A port can be selected
explicitly when required:

```sh
python3 scripts/capture_pcm1802_usb.py pcm1802-raw.wav --port /dev/cu.usbmodem1234
```

Use `--seconds 30` for a timed capture. Otherwise press Ctrl-C to stop. The host
reads the negotiated width and creates a standard 48 kHz stereo signed 16-bit
or 24-bit PCM WAV file to match the firmware. MP3 is avoided because lossy
encoding makes clipping and corruption diagnosis less reliable.

## Stream guardrails

Opening the serial port in the middle of an old byte stream is safe:

1. The device remains idle until the host creates a random session ID and
   completes `INFO -> HELLO -> START -> STARTED`.
2. Starting a session purges stale device TX data and resets sequence/frame
   counters before capture begins.
3. Every packet has an eight-byte magic value, protocol version, packet type,
   session ID, sequence number, source-frame index, payload length, cumulative
   DMA-drop count, and CRC32.
4. The host searches for packet magic after arbitrary garbage, rejects bad
   lengths and CRCs, and writes audio only from its acknowledged session.
5. Source-frame gaps are reported and represented as silence in the WAV so its
   timeline remains correct. Overlaps are trimmed. The final integrity summary
   reports DMA drops, serial packet gaps, CRC failures, discarded bytes, and
   exact full-scale sample counts.

The protocol parser can be tested without a board or pyserial:

```sh
python3 scripts/capture_pcm1802_usb.py --self-test
```

## Interpreting the result

- Distortion in this raw WAV points to the analog feed, PCM1802 configuration,
  clock/power/layout, or I2S capture format.
- A clean raw WAV while normal HDMI output distorts moves the investigation to
  the normal firmware path or HDMI transport.
- A clean, zero-drop 16-bit capture on a host that drops 24-bit frames confirms
  a USB throughput bottleneck. Zero drops with audible corruption instead moves
  the investigation upstream of USB.
- This firmware does not capture the final HDMI audio stream. A second external
  HDMI recording is still needed for a sample-aligned raw-versus-HDMI comparison.
