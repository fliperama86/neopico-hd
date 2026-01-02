# NeoPico-HD

Digital video and audio capture with HDMI output for Neo Geo MVS arcade hardware using Raspberry Pi Pico 2 (RP2350B).

## Features

- **Native 240p HDMI output** at 60fps (via 480p line doubling for audio compatibility)
- **15-bit RGB capture** (32,768 colors) from MVS digital video bus
- **Digital audio capture** from I2S bus (before DAC) with 48kHz HDMI output
- **Zero-overhead DMA video capture** - uses PIO + DMA with ping-pong buffering for perfect stability
- **High-quality audio pipeline** - includes DC blocking, lowpass filtering, and Sample Rate Conversion (SRC)
- **Built-in OSD Foundation** - 5x7 "Mini" font renderer for real-time diagnostics and future menu system

## Status

| Feature             | Status  |
| ------------------- | ------- |
| 480p HDMI video     | Working |
| 60fps capture       | Working |
| 15-bit RGB (R5G5B5) | Working |
| HDMI audio (48kHz)  | Working |
| OSD Diagnostics     | Working |
| RT4K / Tink 4K      | Tested  |

## Hardware Requirements

- **Raspberry Pi Pico 2** - Must be RP2350**B** variant (48 GPIOs) for digital audio capture
- **Neo Geo MVS board** - Tested on MV1C
- **HDMI connector** wired to Pico GPIO

### Hardware Setup & Signal Integrity

To ensure clean audio and video capture, follow these best practices:

1.  **Common Ground**: A solid ground connection between the MVS board and the Pico is **mandatory**.
2.  **Cable Separation**: Physically separate the audio wires (Bank 1) from the video wire bundle to avoid coupling.
3.  **Shielding**: Use a **GND-Signal-GND** pattern when using ribbon cables.
4.  **Series Termination**: Add **33Ω resistors** in series with PCLK and BCK to suppress ringing.

### Pin Configuration

#### Video Capture (Bank 0)

| Function   | GPIO       | Notes                 |
| ---------- | ---------- | --------------------- |
| MVS PCLK   | GPIO 25    | 6 MHz pixel clock     |
| MVS Pixels | GPIO 26-42 | RGB555 contiguous bus |
| MVS CSYNC  | GPIO 43    | Composite sync        |

#### DVI Output (Bank 0/1)

| Function | GPIO       |
| -------- | ---------- |
| DVI CLK  | GPIO 12-13 |
| DVI D0   | GPIO 14-15 |
| DVI D1   | GPIO 16-17 |
| DVI D2   | GPIO 18-19 |

#### Audio Capture (Bank 0 - Low noise)

| Function | GPIO   | MV1C Tap Point |
| -------- | ------ | -------------- |
| I2S DAT  | GPIO 0 | R91            |
| I2S WS   | GPIO 1 | R90            |
| I2S BCK  | GPIO 2 | R92            |

## Building

Requires [Pico SDK](https://github.com/raspberrypi/pico-sdk) with `PICO_SDK_PATH` set.

```bash
# Build main targets
./scripts/build.sh

# Specific targets
make neopico_hd          # Main firmware (Full Cinema Mode)
make dvi_audio_test      # HDMI output test with sine tone
make gpio_freq_analyzer  # Debug tool for wiring verification
```

## Architecture

```
Core 0: Capture & Audio             Core 1: DVI Output
+--------------------------+       +--------------------------+
| Video: PIO -> DMA (PP)   |       | scanline_callback()      |
| Audio: PIO -> DMA -> SRC |       | - 480p line doubling     |
| Main: OSD & Logic        |       | - DMA to TMDS encoders   |
+--------------------------+       +--------------------------+
          |                                   |
          +--------- g_framebuf --------------+
                  (320x240 shared)
```

## Debugging / OSD

Enable the real-time audio debug screen in `src/main.c`:

```c
#define DEBUG_AUDIO_INFO 1
```

This replaces the video output with clear, 1x-scaled 5x7 text showing:

- **GP1/GP2 Activity**: Direct silicon probe of the audio pins.
- **LRCK (MEAS)**: Actual measured sample rate (Target: 55.5kHz).
- **PIO PC**: Internal state machine program counter.
- **DMA ADDR**: Live memory address being written by the audio DMA.

## License

Unlicense
