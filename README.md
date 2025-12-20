# NeoPico-HD

Digital video and audio capture with HDMI output for Neo Geo MVS arcade hardware using Raspberry Pi Pico 2 (RP2350B).

## Features

- **Native 240p HDMI output** at 60fps
- **15-bit RGB capture** (32,768 colors) from MVS digital video bus
- **Digital audio capture** from I2S bus (before DAC) with 48kHz HDMI output
- **Line-by-line processing** - minimal latency, no frame buffer lag
- **Perfect for retro HDMI scalers** - RetroTINK 4K, OSSC PRO, Morth4K

## Status

| Feature             | Status  |
| ------------------- | ------- |
| 240p HDMI video     | Working |
| 60fps capture       | Working |
| 15-bit RGB (R5G5B5) | Working |
| HDMI audio (48kHz)  | Working |
| RT4K compatible     | Tested  |

## Hardware Requirements

- **Raspberry Pi Pico 2** - Must be RP2350**B** variant (48 GPIOs) for audio capture
- **Neo Geo MVS board** - Tested on MV1C
- **HDMI connector** wired to Pico GPIO

### Hardware Setup & Signal Integrity

To ensure clean audio and video capture, follow these best practices:

1.  **Common Ground**: A solid ground connection between the MVS board and the Pico is **mandatory**. Lack of a shared ground reference is the primary cause of audio "harshness" and missing low-level sound data.
2.  **Cable Separation**: High-speed video signals (6MHz PCLK) can broadcast noise into audio lines. Physically separate the audio wires (Bank 1) from the video wire bundle.
3.  **Shielding**: Use a **GND-Signal-GND** pattern when using ribbon cables (e.g., Wire 1: GND, Wire 2: BCK, Wire 3: GND).
4.  **Series Termination**: Add **33Ω resistors** in series with the MVS PCLK and BCK signals (close to the MVS board) to suppress ringing and EMI.
5.  **Thick GND**: Use multiple or thick GND wires to prevent "Ground Bounce" when 15 bits of video data switch simultaneously.

### Pin Configuration

#### Video Capture (Active Active Active)

| Function  | GPIO       | Notes                  |
| --------- | ---------- | ---------------------- |
| MVS PCLK  | GPIO 0     | 6 MHz pixel clock      |
| MVS G4-G0 | GPIO 1-5   | Green channel (5 bits) |
| MVS B0-B4 | GPIO 6-10  | Blue channel (5 bits)  |
| MVS R0-R4 | GPIO 11-15 | Red channel (5 bits)   |
| MVS CSYNC | GPIO 22    | Composite sync         |

#### DVI Output

| Function | GPIO       |
| -------- | ---------- |
| DVI D0   | GPIO 25-26 |
| DVI D1   | GPIO 27-28 |
| DVI D2   | GPIO 29-30 |
| DVI CLK  | GPIO 31-32 |

#### Audio Capture (Bank 1 - isolated from video noise)

| Function | GPIO    | MV1C Tap Point         |
| -------- | ------- | ---------------------- |
| I2S DAT  | GPIO 36 | R91                    |
| I2S WS   | GPIO 37 | R90                    |
| I2S BCK  | GPIO 38 | R92                    |
| GND      | -       | Common ground required |

Tap points are on the IC4 (BU9480F) side of the resistors.

### MV1C Audio Format

The MV1C uses **right-justified I2S** with 16-bit linear PCM:

- **WS HIGH = LEFT channel** (opposite of standard I2S)
- **WS LOW = RIGHT channel**
- 24 BCK cycles per channel, data right-justified
- Sample rate: ~55.5 kHz (8MHz / 144)
- No special decoding needed (unlike YM3016 floating-point format)

## Building

Requires [Pico SDK](https://github.com/raspberrypi/pico-sdk) with `PICO_SDK_PATH` environment variable set.

```bash
# Build all targets
./scripts/build.sh

# Or manually:
mkdir build && cd build && cmake .. && make

# Specific targets
make neopico_hd          # Main firmware (video + audio capture → HDMI)
make audio_pipeline_test # Audio capture test (color bars + audio status)
make dvi_test            # HDMI output test (color bars only, no capture)
make gpio_freq_analyzer  # GPIO/clock analyzer (debug wiring)
```

### RP2350B Board Configuration

GPIO 30-47 only exist on RP2350B. Verify the correct board is configured:

```bash
grep PICO_BOARD build/CMakeCache.txt
# Should show: PICO_BOARD:STRING=weact_studio_rp2350b_core
# NOT: PICO_BOARD:STRING=pico2 (RP2350A has no GPIO 30+)

# Fix stale cache:
rm -rf build && mkdir build && cd build && cmake .. && make
```

## Flashing

```bash
# Auto-reboot and flash
picotool load -f build/src/neopico_hd.uf2

# Or copy UF2 to Pico in BOOTSEL mode
```

## Architecture

```
Core 0: MVS Capture                Core 1: DVI Output
+--------------------------+       +--------------------------+
| Wait for vsync           |       | scanline_callback()      |
| For each line:           |       | - Returns row pointers   |
|   Read 320 pixels (PIO)  |       | - Runs at 60Hz           |
|   Convert RGB555->RGB565 |       | - DMA to TMDS encoders   |
|   Write to framebuffer   |       |                          |
+--------------------------+       +--------------------------+
          |                                   |
          +--------- g_framebuf --------------+
                  (320x240 shared)

Audio Pipeline (runs on Core 0):
+----------+     +--------+     +-----+     +------+
| I2S PIO  | --> |  DMA   | --> | SRC | --> | HDMI |
| 55.5kHz  |     | Buffer |     | 48k |     | Audio|
+----------+     +--------+     +-----+     +------+
```

### Key Design Decisions

1. **240p output** - Native resolution, let external scalers handle upscaling
2. **Line-by-line capture** - No frame buffering, minimal latency
3. **DMA audio capture** - Eliminates FIFO overflow, captures full 55.5kHz
4. **Dual-core** - Video capture and DVI output run independently
5. **Bank 1 audio GPIOs** - Isolated from video switching noise

## HDMI Audio Notes

HDMI audio requires blank lines for data packets, reducing visible area:

```c
// 480 - 16 blank lines = 464 DVI lines = 232 user scanlines
dvi_get_blank_settings(&dvi0)->top = 8;
dvi_get_blank_settings(&dvi0)->bottom = 8;
#define FRAME_HEIGHT 232  // Not 240!
```

## Debugging

```bash
# USB serial monitor
screen /dev/tty.usbmodem* 115200

# Python frame viewer (requires pygame, pyserial)
cd viewer && python3 main.py
```

## Project Structure

```
src/
  main.c                # Main firmware (neopico_hd)
  dvi_test.c            # HDMI output test
  audio_pipeline_test.c # Audio capture test
  gpio_freq_analyzer.c  # GPIO/clock analyzer
  neopico_config.h      # Pin definitions
  mvs_capture.pio       # Video capture PIO program
  audio/
    i2s_capture.pio     # Audio capture PIO program
    i2s_capture.c       # DMA-based I2S capture
    audio_pipeline.c    # Audio processing orchestrator
    src.c               # Sample rate conversion (55.5k → 48k)
    dc_filter.c         # DC blocking filter
    lowpass.c           # Anti-aliasing lowpass filter
docs/
  MVS_MV1C_DIGITAL_VIDEO.md  # Video signal specification
  MVS_MV1C_DIGITAL_AUDIO.md  # Audio signal specification
  CPS2_DIGIAV_REFERENCE.md   # FPGA A/V processing reference
  DVI_PIN_TESTING.md         # RP2350 DVI constraints
  PCB_DESIGN_GUIDE.md        # Hardware design notes
lib/
  PicoDVI/              # DVI output library (submodule)
reference/
  cps2_digiav/          # FPGA reference for Neo Geo audio
```

## Documentation

Detailed technical specifications are in `docs/`:

- **[MVS_MV1C_DIGITAL_VIDEO.md](docs/MVS_MV1C_DIGITAL_VIDEO.md)** - Video signal timing, RGB format, CSYNC structure, tap points
- **[MVS_MV1C_DIGITAL_AUDIO.md](docs/MVS_MV1C_DIGITAL_AUDIO.md)** - Audio format (right-justified I2S), WS polarity, sample rate
- **[CPS2_DIGIAV_REFERENCE.md](docs/CPS2_DIGIAV_REFERENCE.md)** - How cps2_digiav FPGA processes Neo Geo A/V
- **[DVI_PIN_TESTING.md](docs/DVI_PIN_TESTING.md)** - RP2350 DVI pin constraints and solutions
- **[PCB_DESIGN_GUIDE.md](docs/PCB_DESIGN_GUIDE.md)** - Hardware design considerations
- **[HSTX_AUDIO_RESEARCH.md](docs/HSTX_AUDIO_RESEARCH.md)** - RP2350 HSTX as PicoDVI alternative

## References

- [PicoDVI](https://github.com/Wren6991/PicoDVI) - DVI/HDMI output library
- [PicoDVI-N64](https://github.com/kbeckmann/PicoDVI-N64) - Architecture inspiration
- [cps2_digiav](https://github.com/marqs85/cps2_digiav) - Neo Geo audio format reference

## License

Unlicense
