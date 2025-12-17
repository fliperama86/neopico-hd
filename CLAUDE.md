# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

NeoPico-HD captures 15-bit RGB video from Neo Geo MVS arcade hardware and outputs 240p HDMI at 60fps using a Raspberry Pi Pico 2 (RP2350). The project is production-ready.

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
MVS Capture:                    DVI Output:
GPIO 0:     MVS PCLK (6 MHz)    GPIO 25-26: DVI D0
GPIO 1-5:   MVS G4-G0 (Green)   GPIO 27-28: DVI D1
GPIO 6-10:  MVS B0-B4 (Blue)    GPIO 29-30: DVI D2
GPIO 11-15: MVS R0-R4 (Red)     GPIO 31-32: DVI CLK
GPIO 22:    MVS CSYNC
```

See `src/neopico_config.h` for pin definitions and `docs/DVI_PIN_TESTING.md` for RP2350 constraints.

## Critical Constraints

1. **PIO timing is sensitive** - `mvs_capture.pio` contains timing-critical code; changes require hardware testing
2. **240p timing is fixed** - DVI runs at 640×240 @ 60Hz with 126MHz pixel clock
3. **Memory budget** - ~150KB framebuffer on 520KB SRAM; avoid large buffers
4. **No frame buffering** - Line-by-line capture for <1 frame latency
5. **Core independence** - Capture (Core 0) and output (Core 1) must not block each other

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
- `cps2_digiav/` - FPGA-based digital A/V for CPS2/Neo Geo (YM2610 audio handling reference)
- `PicoDVI-N64/` - N64 HDMI implementation using PicoDVI
