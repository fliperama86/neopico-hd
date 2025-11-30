# NeoPico-HD - Project Status

**Last Updated**: 2025-11-30

## Quick Summary

| Item | Status |
|------|--------|
| **Project** | Neo Geo MVS HDMI output using Raspberry Pi Pico 2 |
| **Progress** | 100% - Production ready! |
| **Output** | 240p HDMI at 60fps |
| **Colors** | 15-bit RGB (32,768 colors) |
| **Scaler tested** | RetroTINK 4K |

## Milestones

### Phase 1: Signal Validation ✅
- Measured 6 MHz pixel clock
- Decoded CSYNC structure
- Validated MVS timing matches cps2_digiav specs

### Phase 2: Sync Decoding ✅
- PIO-based horizontal counter
- VSYNC detection via equalization pulses
- 264 lines/frame, 59.19 fps confirmed

### Phase 3: Pixel Capture ✅
- 1-bit R4 capture validated
- Hardware IRQ synchronization (zero jitter)
- DMA-based full frame capture

### Phase 4: 4-bit RGB ✅
- Solved PIO autopush alignment (32÷4 = 8 pixels)
- 8-color output working
- PPM format streaming via USB

### Phase 5: 15-bit RGB ✅
- Full R5G5B5 capture
- RGB555 → RGB565 conversion
- All color data preserved

### Phase 6: DVI Output ✅
- Integrated PicoDVI library
- Custom 240p timing (640×240 @ 60Hz)
- Line-by-line capture for 60fps
- RT4K compatible

## Current Architecture

```
Core 0: Line-by-Line Capture      Core 1: DVI Output
┌─────────────────────────┐      ┌─────────────────────────┐
│ Wait for vsync          │      │ scanline_callback()     │
│ For each line:          │      │ - Returns row pointers  │
│   Skip blanking         │      │ - Runs at 60Hz          │
│   Read from PIO FIFO    │      │ - Reads from g_framebuf │
│   Convert RGB555→RGB565 │      │                         │
│   Write to framebuffer  │      │ dvi_scanbuf_main_16bpp()│
└─────────────────────────┘      └─────────────────────────┘
```

## Hardware Setup

| GPIO | Function |
|------|----------|
| 0-4 | MVS R0-R4 |
| 5-9 | MVS B0-B4 |
| 10-14 | MVS G0-G4 |
| 15 | GND (dummy bit) |
| 16-21 | DVI data pairs |
| 22 | MVS CSYNC |
| 26-27 | DVI clock pair |
| 28 | MVS PCLK |

## Key Files

| File | Purpose |
|------|---------|
| `src/main_dvi.c` | Main application (240p @ 60fps) |
| `src/mvs_sync.pio` | PIO programs for sync + capture |
| `src/CMakeLists.txt` | Build configuration |

## Performance

| Metric | Value |
|--------|-------|
| Resolution | 320×224 capture, 640×240 output |
| Frame rate | 60fps |
| Color depth | 15-bit (32,768 colors) |
| Latency | <1 frame |
| RAM usage | ~150KB |

## Future Ideas

- USB streaming option (dual output)
- OSD overlay (frame counter, status)
- Scanline filter
- AES support (different timing)

## References

- [PicoDVI](https://github.com/Wren6991/PicoDVI)
- [PicoDVI-N64](https://github.com/kbeckmann/PicoDVI-N64)
- [cps2_digiav](https://github.com/marqs85/cps2_digiav)
