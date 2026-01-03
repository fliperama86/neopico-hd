# NeoPico-HD

Digital video and audio capture with HDMI output for Neo Geo MVS arcade hardware using Raspberry Pi Pico 2 (RP2350B).

## Features

- **Native 240p HDMI output** at 60fps (via 480p line doubling for audio compatibility)
- **15-bit RGB capture** (32,768 colors) from MVS digital video bus
- **Digital audio capture** from I2S bus (before DAC) with 48kHz HDMI output
- **Zero-overhead DMA video capture** - uses PIO + DMA with ping-pong buffering for perfect stability
- **Hardware HSTX encoder** - uses RP2350's native TMDS encoder for efficient HDMI output

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

#### HSTX Output (Bank 0/1)

| Function | GPIO       |
| -------- | ---------- |
| TMDS CLK | GPIO 12-13 |
| TMDS D0  | GPIO 14-15 |
| TMDS D1  | GPIO 16-17 |
| TMDS D2  | GPIO 18-19 |

#### Audio Capture (Bank 0 - Low noise)

| Function | GPIO   | MV1C Tap Point |
| -------- | ------ | -------------- |
| I2S DAT  | GPIO 0 | R91            |
| I2S WS   | GPIO 1 | R90            |
| I2S BCK  | GPIO 2 | R92            |

## Building

Requires [Pico SDK](https://github.com/raspberrypi/pico-sdk) with `PICO_SDK_PATH` set.

```bash
# Build and flash
./flash
```

## Architecture

```
Core 0: Video Capture               Core 1: Audio Pipeline + HSTX
+--------------------------+       +--------------------------+
| Video: PIO1 -> DMA (PP)  |       | Audio: PIO2 -> processing|
| Main loop: Control       |       | HSTX hardware encoder    |
| Heartbeat LED            |       | - 640x480 @ 60Hz         |
+--------------------------+       | - HDMI Data Islands      |
          |                        +--------------------------+
          |                                   |
          +--------- framebuf ----------------+
                  (320x240 RGB565)
```

## Documentation

- [HDMI & HSTX Implementation](docs/HDMI_HSTX_IMPLEMENTATION.md)
- [MVS Digital Video Specs](docs/MVS_MV1C_DIGITAL_VIDEO.md)
- [MVS Digital Audio Specs](docs/MVS_MV1C_DIGITAL_AUDIO.md)

## License

Unlicense
