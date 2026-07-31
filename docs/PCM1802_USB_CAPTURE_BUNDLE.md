# NeoPico PCM1802 USB Capture Diagnostic

This package records the PCM1802 serial output directly as signed stereo PCM.
The PCM1802 and RP2350 capture path remain 24-bit. Depending on the included
UF2, USB packets contain either all 24 bits or the upper 16 bits for a
reduced-bandwidth transport test. It bypasses Neo Geo video capture, HSTX, HDMI
audio, sample-rate conversion, volume, EQ, and filters.

## Validation status

The 24-bit firmware has passed source checks, a clean RP2350 build, firmware
metadata inspection, the host protocol self-test, and a local zero-drop 48 kHz
hardware capture. USB throughput remains host-dependent, which is why the
reduced-bandwidth 16-bit variant exists.

## Package contents

- `neopico_pcm1802_usb_capture.uf2`: standalone RP2350B firmware
- `capture_pcm1802_usb.py`: framed capture and WAV writer
- `SHA256SUMS.txt`: checksums for the README, host script, and UF2

The ZIP may be accompanied by a `.zip.sha256` file that verifies the complete
archive before extraction.

## Power and wiring safety

Before connecting USB:

- Prevent backfeeding between USB 5 V and the installation's external 5 V.
  Disconnect external power unless the hardware has verified power-path
  isolation.
- The PCM1802 and RP2350B must share a solid ground during capture.
- Do not alter PCM1802 or RP2350 wiring while either board is powered.

Expected signals:

| Signal | RP2350B pin |
| --- | --- |
| PCM1802 DOUT | GP22 |
| PCM1802 LRCK | GP23 |
| PCM1802 BCK | GP24 |

The expected source format is standard Philips I2S, 24-bit, with LRCK low for
left and LRCK high for right. With a 12.288 MHz SCKI in 256fs master mode, LRCK
is 48 kHz and BCK is 3.072 MHz.

## Flash the included UF2

Flashing this diagnostic temporarily replaces the normal NeoPico-HD firmware.

1. Disconnect external power unless verified power-path isolation is present.
2. Put the RP2350B board into BOOTSEL mass-storage mode.
3. Copy `neopico_pcm1802_usb_capture.uf2` to the BOOTSEL drive.
4. Wait for the board to reboot and enumerate as `NeoPico PCM1802 Capture`.

A trusted RP2350 UF2 flashing tool may be used instead of drag-and-drop.

## Verify and capture

Install Python 3 and the only host dependency:

```sh
python3 -m pip install pyserial
```

From the extracted package directory, run the protocol self-test:

```sh
python3 capture_pcm1802_usb.py --self-test
```

Capture until Ctrl-C:

```sh
python3 capture_pcm1802_usb.py pcm1802-raw.wav
```

Capture for a fixed duration:

```sh
python3 capture_pcm1802_usb.py pcm1802-raw.wav --seconds 30
```

The script checks the USB VID/PID and product descriptor where the operating
system exposes it. Select the serial port explicitly if required:

```sh
python3 capture_pcm1802_usb.py pcm1802-raw.wav --port /dev/cu.usbmodem1234
```

Windows ports use names such as `COM5`.

The capture banner reports whether the included firmware is transmitting 16-bit
or 24-bit samples, and the WAV width is selected automatically.

## Interpret the result

- Distortion in the raw WAV points to the analog feed, PCM1802 configuration,
  clock/power/layout, or I2S capture format.
- A clean raw WAV while HDMI output distorts moves the investigation to the
  normal firmware audio path or HDMI transport.
- This firmware does not capture final HDMI audio. Use an external HDMI
  recording for a raw-versus-HDMI comparison.
- The WAV is labeled 48 kHz. This diagnostic does not independently measure
  LRCK frequency.

The final script summary reports DMA drops, serial packet gaps, CRC failures,
discarded bytes, inserted silence, overlap trimming, sample ranges, and exact
full-scale counts. Zero integrity errors are expected.

## Restore normal NeoPico-HD operation

After collecting the WAV, flash the normal NeoPico-HD UF2 again.

## Check package integrity

On macOS or Linux:

```sh
shasum -a 256 -c SHA256SUMS.txt
```
