# NeoPico PCM1802 USB Capture Source

This is a standalone source package for the NeoPico PCM1802 USB capture
diagnostic. It builds without the NeoPico-HD video or PicoHDMI sources.

The firmware captures standard-I2S PCM1802 DOUT as signed, stereo, 24-bit PCM
and sends versioned, CRC-protected packets through TinyUSB CDC. The default
transport preserves all 24 bits. An optional reduced-bandwidth transport sends
the upper 16 bits, and the included host tool automatically writes a matching
48 kHz stereo WAV file.

## Validation status

The source package is built from the same files as the NeoPico-HD repository.
Its standalone CMake project is tested after export with clean 24-bit and 16-bit
RP2350 builds. The 24-bit firmware has completed a local zero-drop 48 kHz
hardware capture; achievable USB throughput remains host-dependent.

## Source layout

```text
CMakeLists.txt
LICENSE
src/
  main.c
  usb_descriptors.c
  tusb_config.h
  i2s_capture.pio
tools/
  capture_pcm1802_usb.py
```

## Requirements

- Raspberry Pi Pico SDK with RP2350 support
- Arm GNU Toolchain
- CMake and a supported build tool such as Ninja or Make
- WeAct RP2350B board, or another RP2350 board exposing GP22 through GP24

## Build

Set `PICO_SDK_PATH`, then configure and build from this extracted directory:

```sh
cmake -S . -B build \
  -DPICO_SDK_PATH="$HOME/pico-sdk" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --target neopico_pcm1802_usb_capture -j
```

The default board is `weact_studio_rp2350b_core`. Override `PICO_BOARD` and, if
needed, `PICO_PLATFORM` for another RP2350 board.

The standalone UF2 is generated at:

```text
build/neopico_pcm1802_usb_capture.uf2
```

For the reduced-bandwidth diagnostic, add this configure option and use a
separate build directory:

```sh
-DNEOPICO_PCM1802_USB_BITS_PER_SAMPLE=16
```

The default is `24`. The 16-bit option does not reconfigure the PCM1802 or PIO;
it discards the least-significant byte only while staging USB packets.

## Expected PCM1802 connection

| Signal | RP2350B pin |
| --- | --- |
| PCM1802 DOUT | GP22 |
| PCM1802 LRCK | GP23 |
| PCM1802 BCK | GP24 |

Expected format: standard Philips I2S, 24-bit, LRCK low for left and high for
right. With 12.288 MHz SCKI in 256fs master mode, LRCK is 48 kHz and BCK is
3.072 MHz.

## Power safety

Prevent backfeeding between USB 5 V and external 5 V. Disconnect external power
unless the installation has verified power-path isolation. Keep a solid shared
ground between the PCM1802 and RP2350B, and do not alter wiring while powered.

## Host protocol self-test and capture

The self-test does not require pyserial or hardware:

```sh
python3 tools/capture_pcm1802_usb.py --self-test
```

Install pyserial for a real capture:

```sh
python3 -m pip install pyserial
python3 tools/capture_pcm1802_usb.py pcm1802-raw.wav
```

Use `--seconds 30` for a timed capture or `--port COM5` on Windows when explicit
port selection is needed.

Flashing this diagnostic temporarily replaces the normal NeoPico-HD firmware.
Flash the normal NeoPico-HD UF2 again after collecting the WAV.
