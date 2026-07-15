[![Discord](https://img.shields.io/discord/1469422496833863884)](https://discord.gg/Th7HEHTC8p)

# NeoPico-HD

Digital video and audio capture with HDMI output for Neo Geo MVS arcade hardware using Raspberry Pi Pico 2 (RP2350B).

## Features

- **Native 240p HDMI output** at 60fps (via 480p line doubling for audio compatibility)
- **Experimental 720p HDMI output** - `NEOPICO_VIDEO_720P=ON`, centered 3x 4:3 scale; release builds use the non-RT PicoHDMI path for the best 720p stability
- **15-bit RGB + SHADOW/DARK capture** - 19-bit capture path includes SHADOW and DARK control lines
- **Pixel Conversion Modes** - Stable 32K-entry RGB LUT, plus an optional live and persistent `Digital`/`Analog` normal-color selector that remains separate from DARK/SHADOW processing
- **Translucent OSD Panel** - Enabled by default; game pixels under the black panel retain 12.5% brightness while text and menu colors remain opaque
- **Digital audio capture** from I2S bus (before DAC) with 48kHz HDMI output
- **Zero-overhead DMA video capture** - uses PIO + DMA with ping-pong buffering for perfect stability
- **PicoHDMI Output** - Powered by the [PicoHDMI](https://github.com/fliperama86/pico_hdmi) library for efficient, hardware-native TMDS encoding via RP2350 HSTX.

## Status

| Feature                 | Status                                     |
| ----------------------- | ------------------------------------------ |
| 480p HDMI video         | Working                                    |
| 720p HDMI video         | Experimental (`NEOPICO_VIDEO_720P=ON`, non-RT release build) |
| 60fps capture           | Working                                    |
| RGB555 Digital path     | Working                                    |
| Live Colors selector    | Optional and experimental (`NEOPICO_MVS_COLOR_MODEL_MENU=ON`) |
| SHADOW/DARK capture     | Working                                    |
| SHADOW/DARK conversion  | Experimental, default off; split-LUT hardware test showed bottom-screen pixel jitter |
| HDMI audio (48kHz)      | Working                                    |
| OSD Diagnostics         | Working                                    |
| Morhph4K, RetroTink 4K  | Tested                                     |
| Samsung Q80, Acer Pred. | Tested                                     |

## Hardware Requirements

- **Raspberry Pi Pico 2** - Must be RP2350**B** variant (48 GPIOs) for digital audio capture
- **Neo Geo MVS board** - Tested on MV1C
- **HDMI connector** wired to Pico GPIO

### Hardware Setup & Signal Integrity

To ensure clean audio and video capture, follow these best practices:

1.  **Common Ground**: A solid ground connection between the MVS board and the Pico is **mandatory**.
2.  **HDMI Power**: It is **absolutely mandatory** to have the HDMI connector properly powered with **+5V**. While some devices (like the Morph4K) may be forgiving, others (like the RetroTINK, TVs and monitors) will not pick up the signal without it.
3.  **Level Shifting**: Route all MVS -> Pico digital lines through proper **5V-to-3.3V level shifters**.
4.  **Clock Conditioning**: Keep **Schmitt-trigger conditioning** on PCLK and BCK for clean edges.
5.  **Power-Path Isolation**: Prevent back-feed between external 5V and USB 5V (ideal diode/power mux approach recommended).
6.  **Cable Separation**: Physically separate the I2S audio wires (GPIO 22-24 path) from the video wire bundle to avoid coupling.
7.  **Shielding**: Use a **GND-Signal-GND** pattern when using ribbon cables.

### Pin Configuration

#### Video Capture (Bank 1)

| Function   | GPIO       | Notes                |
| ---------- | ---------- | -------------------- |
| MVS CSYNC  | GPIO 27    | Composite sync       |
| MVS PCLK   | GPIO 28    | 6 MHz pixel clock    |
| MVS Blue   | GPIO 29-33 | B4-B0 (contiguous)   |
| MVS Green  | GPIO 34-38 | G4-G0 (contiguous)   |
| MVS Red    | GPIO 39-43 | R4-R0 (contiguous)   |
| MVS SHADOW | GPIO 44    | Shadow dimming       |
| MVS DARK   | GPIO 45    | Dark dimming control |

#### HSTX Output (Bank 0/1)

| Function | GPIO       |
| -------- | ---------- |
| TMDS CLK | GPIO 12-13 |
| TMDS D0  | GPIO 14-15 |
| TMDS D1  | GPIO 16-17 |
| TMDS D2  | GPIO 18-19 |

#### Audio Capture

| Function | GPIO    | MV1C Tap Point |
| -------- | ------- | -------------- |
| I2S DAT  | GPIO 22 | R91            |
| I2S WS   | GPIO 23 | R90            |
| I2S BCK  | GPIO 24 | R92            |

#### Controller OSD Inputs (MVS/AES)

| Controller input | GPIO |
| ---------------- | ---- |
| START / MENU     | GP0  |
| SELECT / BACK    | GP1  |
| DOWN             | GP2  |
| UP               | GP3  |

The inputs are active low and use weak internal pull-ups so untapped pins stay
idle. They default to ON for MVS/AES builds and OFF for SNES builds. Override
either default with `-DNEOPICO_OSD_CONTROLLER_INPUTS=ON` or `OFF`.

## Prebuilt Firmware

GitHub Releases include ready-to-flash selector firmware. Each UF2 can switch
between 240p, 480p, and 720p from the reboot-based OSD resolution menu. The MVS
asset also includes persistent Audio and live-preview Colors menus.

| Asset | Capture | OSD choices |
| ----- | ------- | ----------- |
| `neopico_hd_mvs.uf2` | Neo Geo MVS/AES | Resolution, Audio, and Digital/Analog Colors |
| `neopico_hd_snes.uf2` | SNES | Resolution |

Matching ELF files and the `neopico-hd-jlcpcb.zip` fabrication package are also
attached to each release. Controller-driven AES OSD navigation is enabled in
the MVS release asset; the SNES asset leaves those inputs disabled.

## Building

Requires [Pico SDK](https://github.com/raspberrypi/pico-sdk) with `PICO_SDK_PATH` set.

```bash
# Build and flash
./flash

# Experimental 720p non-RT build
cmake -S . -B build_720p_nonrt -DNEOPICO_VIDEO_720P=ON -DNEOPICO_USE_NONRT_HDMI=ON
cmake --build build_720p_nonrt --target neopico_hd -j4

# Optional standalone HDMI/OSD self-test firmware
cmake -S . -B build_selftest -DNEOPICO_BUILD_SELFTEST=ON
cmake --build build_selftest --target neopico_selftest -j4

# MVS/AES universal build with the persistent Audio menu and AES controller OSD
cmake -S . -B build_audio_menu \
  -DNEOPICO_AUDIO_MODE=SELECTABLE
cmake --build build_audio_menu --target neopico_hd -j4

# Fixed-source AES build without the Audio menu
cmake -S . -B build_pcm1802 -DNEOPICO_AUDIO_MODE=PCM1802
cmake --build build_pcm1802 --target neopico_hd -j4

# Live persistent normal-color selector. The OSD values are Digital (stable
# default) and Analog (experimental). Moving previews live, SELECT reverts, and
# START confirms without rebooting. Both choices ignore DARK/SHADOW.
cmake -S . -B build_color_menu \
  -DNEOPICO_MVS_COLOR_MODEL_MENU=ON \
  -DNEOPICO_ENABLE_DARK_SHADOW=OFF
cmake --build build_color_menu --target neopico_hd -j4

# Separate DARK/SHADOW timing experiment. This path produced bottom-screen
# pixel jitter in hardware testing and must not be combined with the color menu.
cmake -S . -B build_effects \
  -DNEOPICO_ENABLE_DARK_SHADOW=ON \
  -DNEOPICO_MVS_COLOR_MODEL_MENU=OFF \
  -DNEOPICO_MVS_EFFECT_MODEL=MISTER
cmake --build build_effects --target neopico_hd -j4
```

## Architecture

```
Core 0: Video Capture               Core 1: Audio Pipeline + HSTX
+--------------------------+       +--------------------------+
| Video: PIO1 -> DMA (PP)  |       | Audio: PIO2 -> processing|
| Conv: 32K LUT (dual opt) |       | [ PicoHDMI Library ]     |
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
- **[Reboot Resolution Switching](docs/REBOOT_RESOLUTION_SWITCHING.md)**: Stable 480p/240p/720p BACK-button cycler notes.
- **[720p Samsung Game Mode Investigation](docs/720P_SAMSUNG_GAME_MODE_INVESTIGATION.md)**: Current findings on Samsung Game Mode 720p glitches.
- **[Known Issues](docs/KNOWN_ISSUES.md)**: Current limitations and compatibility notes.

## Architecture Highlights

- **Dual-Core Design**: Dedicated cores for Video Capture (Core 0) and HDMI Output (Core 1).
- **Closed-Loop Audio Sync**: Software-defined feedback loop effectively "genlocks" the MVS audio to the HDMI clock, preventing drift and glitches without an FPGA.
- **Zero-Lag**: Scanline-doubling architecture with no framebuffer delay.

## Installation

Unlicense
