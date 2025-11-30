# NeoPico-HD - Neo Geo MVS HDMI Output

Digital video capture and HDMI output for Neo Geo MVS arcade hardware using Raspberry Pi Pico 2 (RP2350).

## Features

- **Native 240p HDMI output** at 60fps
- **15-bit RGB capture** (32,768 colors) from MVS digital video bus
- **Line-by-line processing** - minimal latency, no frame buffer lag
- **Perfect for retro scalers** - RT4K, OSSC, RetroTINK

## Current Status

| Feature | Status |
|---------|--------|
| 240p HDMI output | ✅ Working |
| 60fps capture | ✅ Working |
| 15-bit RGB (R5G5B5) | ✅ Working |
| Horizontal stability | ✅ Fixed |
| RT4K compatible | ✅ Tested |

## Hardware Requirements

- Raspberry Pi Pico 2 (RP2350)
- Neo Geo MVS board (tested on MV1C)
- HDMI connector wired to Pico

### Pin Configuration

| Function | GPIO | Notes |
|----------|------|-------|
| MVS R0-R4 | GPIO 0-4 | Red channel (5 bits) |
| MVS B0-B4 | GPIO 5-9 | Blue channel (5 bits) |
| MVS G0-G4 | GPIO 10-14 | Green channel (5 bits) |
| MVS GND | GPIO 15 | Dummy bit for 16-bit alignment |
| DVI Data | GPIO 16-21 | 3 differential pairs (active active active) |
| MVS CSYNC | GPIO 22 | Composite sync |
| DVI Clock | GPIO 26-27 | Differential clock pair |
| MVS PCLK | GPIO 28 | 6 MHz pixel clock |

### MVS Hookup Points (MV1C Board)

- **CSYNC**: R51 (active bottom side)
- **PCLK**: active active active
- **R0-R4**: active active active
- **G0-G4**: active active active
- **B0-B4**: active active active
- **GND**: active active active

## Building

```bash
mkdir build && cd build
cmake ..
make main_dvi
```

## Flashing

```bash
picotool load -f build/src/main_dvi.uf2
```

Or copy `main_dvi.uf2` to the Pico when in BOOTSEL mode.

## Architecture

```
Core 0: MVS Capture                Core 1: DVI Output
┌─────────────────────────┐       ┌─────────────────────────┐
│ Wait for vsync          │       │ scanline_callback()     │
│ For each line:          │       │ - Returns row pointers  │
│   Skip blanking         │       │ - Runs at 60Hz          │
│   Read 320 pixels       │       │ - Reads from g_framebuf │
│   Convert RGB555→RGB565 │       │                         │
│   Write to framebuffer  │       │ dvi_scanbuf_main_16bpp()│
└─────────────────────────┘       └─────────────────────────┘
          │                                   │
          └──────── g_framebuf ───────────────┘
                 (320×240 shared buffer)
```

### Key Design Decisions

1. **240p output** - Native resolution, let the scaler handle upscaling
2. **Line-by-line capture** - No DMA buffer, process pixels as they arrive
3. **Single framebuffer** - Accepts minor tearing for simpler design
4. **Dual-core** - Capture and output run independently

## Performance

| Metric | Value |
|--------|-------|
| Capture resolution | 320×224 |
| Output resolution | 640×240 (240p) |
| Color depth | 15-bit RGB |
| Frame rate | 60fps |
| RAM usage | ~150KB (framebuffer only) |

## References

- [PicoDVI](https://github.com/Wren6991/PicoDVI) - DVI/HDMI output library
- [PicoDVI-N64](https://github.com/kbeckmann/PicoDVI-N64) - Architecture inspiration
- [cps2_digiav](https://github.com/marqs85/cps2_digiav) - MVS timing constants

## License

MIT License
