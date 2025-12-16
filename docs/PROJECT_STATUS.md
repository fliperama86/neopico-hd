# NeoPico-HD - Project Status

**Last Updated**: 2025-12-16

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
| 0 | MVS PCLK |
| 1-5 | MVS G4-G0 (reversed) |
| 6-10 | MVS B0-B4 |
| 11-15 | MVS R0-R4 |
| 22 | MVS CSYNC |
| 25-26 | DVI D0 |
| 27-28 | DVI D1 |
| 29-30 | DVI D2 |
| 31-32 | DVI CLK |

See `docs/DVI_PIN_TESTING.md` for RP2350 PIO/PWM constraints.

## Key Files

| File | Purpose |
|------|---------|
| `src/main.c` | Main application (240p @ 60fps) |
| `src/main_usb.c` | USB streaming variant |
| `src/mvs_capture.pio` | PIO programs for sync + capture |
| `src/neopico_config.h` | Shared DVI/MVS pin configuration |
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

- Audio capture (see below)
- OSD overlay (frame counter, status)
- Scanline filter
- AES support (different timing)

## Audio Capture (Not Yet Implemented)

The Neo Geo MVS uses a **Yamaha YM2610** sound chip which outputs **analog audio only**. There is no digital audio bus to tap like the RGB video bus.

### Approach: External I2S ADC

The cps2_digiav project solves this with a small daughter board (`neogeo_aadc`) containing:

- **WM8782** stereo ADC (analog → I2S digital)
- 3.3V regulator (TLV70033)
- Input filtering and decoupling

```
MVS Analog Audio (L/R)
        ↓
   [I2S ADC chip] ← MCLK from Pico
        ↓
   I2S (16-bit stereo, ~48kHz)
        ↓
   Pico PIO (3 GPIOs: DATA, WS, BCK)
        ↓
   PicoDVI audio embedding
        ↓
   HDMI audio stream
```

### Hardware Required

| Component | Purpose | Notes |
|-----------|---------|-------|
| I2S ADC module | Digitize analog audio | PCM1802, WM8782, or similar |
| 3 GPIO pins | I2S signals | DATA, WS (LRCLK), BCK |
| 1 GPIO pin | MCLK output | Master clock to ADC |

### GPIO Options

Current pinout leaves these available:
- GPIO 23-25: Free (could use for I2S)
- GPIO 29: ADC3 (for Pico's built-in ADC, mono only)

### Alternative: Pico's Built-in ADC

Simpler but lower quality:
- 12-bit resolution (vs 16-bit I2S ADC)
- GPIO 26-27 conflict with DVI clock
- Could do mono on GPIO 29

### Reference Implementation

See `reference/cps2_digiav/`:
- `pcb_neogeo_aadc/` - KiCad schematic for ADC board
- `rtl_common/i2s_rx_asrc.v` - I2S receiver logic
- `board/neogeo/rtl/i2s_upsampler_asrc.v` - Audio processing pipeline

## References

- [PicoDVI](https://github.com/Wren6991/PicoDVI)
- [PicoDVI-N64](https://github.com/kbeckmann/PicoDVI-N64)
- [cps2_digiav](https://github.com/marqs85/cps2_digiav)
