# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

NeoPico-HD captures pure digital-to-digital 15-bit RGB video audio (BCK, DAT, WS before DAC) from Neo Geo MVS arcade hardware and outputs 240p HDMI at 60fps using a Raspberry Pi Pico 2 (RP2350B variant with 48 GPIOs).

## Build Commands

```bash
# Build (requires PICO_SDK_PATH environment variable)
./scripts/build.sh

# Or manually:
mkdir build && cd build && cmake .. && make neopico_hd

# Build specific targets
make neopico_hd  # Main app (MVS capture + DVI output)
make neopico_usb # USB streaming variant (for PC viewer)
make dvi_test    # DVI color bar test
make gpio_test   # GPIO wiring test

# Deploy to Pico (auto-detects BOOTSEL mode)
./scripts/deploy.sh

# Or flash directly
picotool load -f build/src/main_dvi.uf2
```

## Architecture

**Dual-core design with shared framebuffer:**

- **Core 0**: MVS video capture - waits for VSYNC, reads pixels from PIO FIFO, converts RGB555→RGB565, writes to framebuffer
- **Core 1**: DVI output - runs scanline callback at 60Hz, reads from shared framebuffer via DMA

Key files:

- `src/main.c` - Main application (DVI output), dual-core setup, capture loop
- `src/main_usb.c` - USB streaming variant (streams to PC viewer)
- `src/mvs_capture.pio` - PIO state machines for sync detection and pixel capture
- `src/neopico_config.h` - Shared pin configuration for DVI and MVS
- `lib/PicoDVI/` - DVI output library (git submodule)

## Hardware Configuration

```
MVS Video Capture:              DVI Output:
GPIO 0:     MVS PCLK (6 MHz)    GPIO 25-26: DVI D0
GPIO 1-5:   MVS G4-G0 (Green)   GPIO 27-28: DVI D1
GPIO 6-10:  MVS B0-B4 (Blue)    GPIO 29-30: DVI D2
GPIO 11-15: MVS R0-R4 (Red)     GPIO 31-32: DVI CLK
GPIO 22:    MVS CSYNC
```

```
MVS Audio Capture (Bank 1 - isolated from video switching noise):
GPIO 36: MVS DAT (I2S data)
GPIO 37: MVS WS  (I2S word select / LRCK)
GPIO 38: MVS BCK (I2S bit clock)

Tap points on MV1C (IC4/BU9480F side of resistors):
R90: WS, R91: DAT, R92: BCK
```

See `src/neopico_config.h` for pin definitions and `docs/DVI_PIN_TESTING.md` for RP2350 constraints.

## Critical Constraints

1. **PIO timing is sensitive** - `mvs_capture.pio` contains timing-critical code; changes require hardware testing
2. **240p timing is fixed** - DVI runs at 640×240 @ 60Hz with 126MHz pixel clock
3. **Memory budget** - ~150KB framebuffer on 520KB SRAM; avoid large buffers
4. **No frame buffering** - Line-by-line capture for <1 frame latency
5. **Core independence** - Capture (Core 0) and output (Core 1) must not block each other

## HDMI Audio with PicoDVI

When enabling HDMI audio with PicoDVI's blank settings, the frame height must be reduced:

```c
// HDMI audio requires blank lines for audio data packets
dvi_get_blank_settings(&dvi0)->top = 4 * 2;    // 8 lines
dvi_get_blank_settings(&dvi0)->bottom = 4 * 2; // 8 lines

// This reduces visible area: 480 - 16 = 464 DVI lines = 232 user scanlines
// FRAME_HEIGHT must be 232, NOT 240, or the display will scroll vertically
#define FRAME_HEIGHT 232
```

**Root cause**: The blank settings reserve DVI lines for audio data packets. With `top=8, bottom=8`, 16 DVI lines are blanked. With 2x pixel doubling, this means 8 fewer user scanlines are consumed per frame. If you provide 240 scanlines but only 232 are consumed, the extra 8 accumulate each frame, causing vertical scrolling.

Audio pipeline files:

- `src/audio/` - Modular audio capture and processing
- `src/audio/i2s_capture.pio` - PIO program for I2S capture
- `src/audio/audio_pipeline.c` - Pipeline orchestrator
- `src/audio_pipeline_test.c` - Test app with color bars + audio

## RP2350B GPIO 30+ (WeAct RP2350B)

GPIO pins 30-47 only exist on RP2350B variants. **Critical**: The CMakeCache must use the correct board definition.

```bash
# If GPIO 30+ don't work, check CMakeCache.txt:
grep PICO_BOARD build/CMakeCache.txt
# Should show: PICO_BOARD:STRING=weact_studio_rp2350b_core
# NOT: PICO_BOARD:STRING=pico2  (this is RP2350A - no GPIO 30+)

# Fix by cleaning and rebuilding:
rm -rf build && mkdir build && cd build && cmake .. && make
```

The board is set in `CMakeLists.txt` but CMake caches the value. A stale cache from a previous build may override it.

**RP2350 Errata E9**: Internal pull-downs have a hardware bug that can cause pins to latch. Use pull-ups for buttons on GPIO 30+, or use external resistors.

## Debugging

```bash
# USB serial monitor
screen /dev/tty.usbmodem* 115200

# Python frame viewer (requires pygame, pyserial)
cd viewer && python3 main.py
```

## Technical Documentation

Detailed timing specs and implementation notes are in `docs/`:

- `MVS_CAPTURE.md` - MVS signal specifications
- `MVS_DIGITAL_VIDEO.md` - Timing analysis
- `PROJECT_STATUS.md` - Architecture overview and milestones
- `DVI_PIN_TESTING.md` - RP2350 DVI pin constraints and solutions

## Reference Implementations

Source code for reference projects is in `reference/`:

- `cps2_digiav/` - FPGA-based digital A/V for CPS2/Neo Geo
- `PicoDVI-N64/` - N64 HDMI implementation using PicoDVI

**IMPORTANT: MV1C Audio Format**

The MV1C uses **right-justified I2S** with 16-bit linear PCM (confirmed from `cps2_digiav/board/neogeo` which uses `i2s_rx_asrc` with MODE=1).

Key differences from standard I2S:
- **WS HIGH = LEFT channel** (opposite of standard I2S where WS LOW = LEFT)
- **WS LOW = RIGHT channel**
- Data is 16-bit signed linear PCM - no special decoding needed
- 24 BCK cycles per channel, data right-justified in the frame

DO NOT use the `ym_rx_asrc` floating-point decoding (exponent+mantissa) - that's for different hardware.

## Aliases

- `CAV` = "cps2_digiav MVS"

## General Rules

- When using terminal output, prefer to send update on the same lines instead printf'n new lines.
