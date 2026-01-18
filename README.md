# NeoPico-HD

Digital video and audio capture with HDMI output for Neo Geo MVS arcade hardware using Raspberry Pi Pico 2 (RP2350B).

## Features

- **Native 240p HDMI output** at 60fps (via 480p line doubling for audio compatibility)
- **15-bit RGB + SHADOW/DARK support** - Accurately reproduces Neo Geo's unique brightness modifiers
- **Hardware-Accelerated Pixel Conversion** - Uses RP2350 Interpolators + 256KB LUT for zero-overhead RGB565 conversion
- **Digital audio capture** from I2S bus (before DAC) with 48kHz HDMI output
- **Zero-overhead DMA video capture** - uses PIO + DMA with ping-pong buffering for perfect stability
- **PicoHDMI Output** - Powered by the [PicoHDMI](https://github.com/fliperama86/pico_hdmi) library for efficient, hardware-native TMDS encoding via RP2350 HSTX.

## Status

| Feature              | Status  |
| -------------------- | ------- |
| 480p HDMI video      | Working |
| 60fps capture        | Working |
| RGB555 + DARK/SHADOW | Working |
| HDMI audio (48kHz)   | Working |
| OSD Diagnostics      | Working |
| RT4K / Tink 4K       | Tested  |

## Hardware Requirements

- **Raspberry Pi Pico 2** - Must be RP2350**B** variant (48 GPIOs) for digital audio capture
- **Neo Geo MVS board** - Tested on MV1C
- **HDMI connector** wired to Pico GPIO

### Hardware Setup & Signal Integrity

To ensure clean audio and video capture, follow these best practices:

1.  **Common Ground**: A solid ground connection between the MVS board and the Pico is **mandatory**.
2.  **HDMI Power**: It is **absolutely mandatory** to have the HDMI connector properly powered with **+5V**. While some devices (like the Morph4K) may be forgiving, others (like the RetroTINK, TVs and monitors) will not pick up the signal without it.
3.  **Cable Separation**: Physically separate the audio wires (Bank 1) from the video wire bundle to avoid coupling.
4.  **Shielding**: Use a **GND-Signal-GND** pattern when using ribbon cables.
5.  **Series Termination**: Add **33Ω resistors** in series with PCLK and BCK to suppress ringing.

### Pin Configuration

#### Video Capture (Bank 1)

| Function    | GPIO       | Notes                   |
| ----------- | ---------- | ----------------------- |
| MVS PCLK    | GPIO 25    | 6 MHz pixel clock       |
| MVS Pixels  | GPIO 26-40 | RGB555 contiguous bus   |
| MVS Dimming | GPIO 41-42 | DARK and SHADOW signals |
| MVS CSYNC   | GPIO 43    | Composite sync          |

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
| Conv: Interp + 256KB LUT |       | [ PicoHDMI Library ]     |
| Main loop: Control       |       | - 640x480 @ 60Hz         |
| Heartbeat LED            |       | - HDMI Data Islands      |
+--------------------------+       +--------------------------+
          |                        +--------------------------+
          |                                   |
          +--------- framebuf ----------------+
                  (320x240 RGB565)
```

### PicoHDMI Library Integration

The project leverages the [PicoHDMI](https://github.com/fliperama86/pico_hdmi) library (found in `lib/pico_hdmi`) to interface with the RP2350's HSTX peripheral. This library provides the high-performance TMDS encoding and Data Island management required for stable HDMI output.

- **Dedicated Output Core**: To ensure rock-solid HDMI timing (exactly 800 cycles per line), the library runs its main loop on **Core 1**. This isolates the high-priority TMDS serialization from the video capture logic on Core 0.
- **Zero-Latency Scaling**: We use the library's `video_output_set_scanline_callback()` to implement a scanline doubler. This performs 2x vertical upscaling (240p to 480p) on-the-fly as pixels are streamed to the HSTX, avoiding the need for a full 480p framebuffer.
- **HDMI Audio Injection**: Audio data is integrated via the library's Data Island queue (`hstx_di_queue`). The audio subsystem pushes TERC4-encoded packets into this queue, which the library then automatically injects during the horizontal blanking intervals.
- **Frame Synchronization**: A VSYNC callback is used to keep the video capture ring buffer and the HDMI output in perfect sync, minimizing latency and preventing screen tearing.

## Documentation

- **[System Architecture](docs/ARCHITECTURE.md)**: High-level design, core partitioning, and the closed-loop audio sync.
- **[Video Implementation](docs/MVS_MV1C_DIGITAL_VIDEO.md)**: Tap points, signal logic, and PIO capture.
- **[Audio Implementation](docs/MVS_MV1C_DIGITAL_AUDIO.md)**: I2S format, ASRC strategy, and drift control.
- **[HSTX & HDMI](docs/HSTX_IMPLEMENTATION.md)**: Output timing, TMDS, and Data Islands.
- **[OSD](docs/OSD_IMPLEMENTATION.md)**: On-Screen Display rendering.
- **[Best Practices](docs/BEST_PRACTICES.md)**: Critical Do's and Don'ts for developers.

## Architecture Highlights

- **Dual-Core Design**: Dedicated cores for Video Capture (Core 0) and HDMI Output (Core 1).
- **Closed-Loop Audio Sync**: Software-defined feedback loop effectively "genlocks" the MVS audio to the HDMI clock, preventing drift and glitches without an FPGA.
- **Zero-Lag**: Scanline-doubling architecture with no framebuffer delay.

## Installation

Unlicense
