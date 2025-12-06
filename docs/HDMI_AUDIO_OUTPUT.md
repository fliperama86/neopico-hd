# HDMI Audio Output Options for RP2350

Research summary for implementing HDMI audio output on RP2350/RP2350B.

## Background

HDMI audio is transmitted via **Data Islands** - special packets inserted during video blanking periods using **TERC4 encoding** (different from TMDS used for video pixels). The RP2350's HSTX peripheral has native TMDS encoding but lacks native TERC4 support.

## Option 1: PIO-based DVI with Audio (Recommended)

**Library**: [ikjordan/PicoDVI](https://github.com/ikjordan/PicoDVI)

Uses PIO state machines to generate DVI video plus HDMI data islands for audio. This is a fork of the original PicoDVI with audio support added by shuichitakano.

| Aspect | Details |
|--------|---------|
| **RP2350 Support** | Yes (uses SIO TMDS encoders) |
| **Audio** | Proven working, 44.1kHz/48kHz stereo |
| **Wiring** | Compatible with existing PicoDVI/Pico-DVI-Sock pinout |
| **GPIO Pins** | ~14 (directly flexible - PIO can use most GPIOs) |
| **CPU Load** | Moderate (PIO handles encoding) |

**Pros:**
- Drop-in replacement for existing PicoDVI setup
- Proven HDMI audio implementation
- No additional hardware cost
- Same wiring as current DVI output

**Cons:**
- Uses PIO resources (1-2 state machines)
- Slightly higher CPU usage than HSTX

**Build command:**
```bash
cmake -DPICO_PLATFORM=rp2350 -DPICO_COPY_TO_RAM=1 ..
```

**Audio examples included:** `moon_double_audio`, `sprite_bounce_audio`, `colour_terminal_audio`

---

## Option 2: HSTX with RAW Mode Audio (Experimental)

**Reference**: [hsdaoh-rp2350](https://github.com/steve-m/hsdaoh-rp2350)

Uses the RP2350's native HSTX peripheral for video, with software-generated TERC4 packets in RAW mode during blanking periods.

| Aspect | Details |
|--------|---------|
| **RP2350 Support** | Yes (native HSTX on GPIO 12-19) |
| **Audio** | Theoretically possible, no public demo |
| **GPIO Pins** | 8 (fixed: GPIO 12-19) |
| **CPU/DMA Load** | High during blanking (RAW mode) |

### How It Works

The HSTX has four command modes (from official pico-examples):
```c
#define CMDLSB_RAW         (0x0u << 12)  // Direct 32-bit output
#define CMDLSB_RAW_REPEAT  (0x1u << 12)  // Repeat raw data N cycles
#define CMDLSB_TMDS        (0x2u << 12)  // Hardware TMDS encoding
#define CMDLSB_TMDS_REPEAT (0x3u << 12)  // Repeat TMDS output
```

- **Video (active pixels)**: Use TMDS mode - hardware encodes RGB to 10-bit TMDS
- **Blanking (audio/control)**: Switch to RAW mode - software provides pre-encoded TERC4 patterns

### The Challenge

RAW mode consumes data 5-10x faster than TMDS mode because you're sending raw 10-bit patterns instead of compressed pixel data. This creates DMA bandwidth pressure:

- If DMA can't keep up, HSTX FIFO underruns → signal glitches
- May require overclocking system clock above HSTX clock (e.g., 300MHz sys vs 252MHz HSTX)
- Pre-computed TERC4 lookup tables needed in RAM

**Pros:**
- Frees PIO for other uses
- Native hardware TMDS encoder (efficient for video)
- Minimal GPIO usage

**Cons:**
- Novel implementation work required
- DMA bandwidth concerns
- May require overclocking for stability
- No working audio demo exists publicly

### HSTX Pinout (Fixed)

| Signal | GPIO | Notes |
|--------|------|-------|
| Lane 2 (D2+/D2-) | 12, 13 | Blue channel |
| Clock (+/-) | 14, 15 | Pixel clock |
| Lane 1 (D1+/D1-) | 16, 17 | Green channel |
| Lane 0 (D0+/D0-) | 18, 19 | Red channel |

---

## Option 3: External HDMI Transmitter IC

Use an external IC that accepts parallel RGB video + I2S audio and outputs compliant HDMI.

### MS7210 (MacroSilicon)

| Aspect | Details |
|--------|---------|
| **Price** | ~$1.50/unit ([LCSC](https://www.lcsc.com/product-detail/C2914249.html)) |
| **Package** | QFN-64 (9x9mm) |
| **Video Input** | Parallel RGB/YUV |
| **Audio Input** | I2S or S/PDIF |
| **Resolution** | Up to 4K@30Hz |

### ADV7511 (Analog Devices)

| Aspect | Details |
|--------|---------|
| **Price** | ~$8-15/unit |
| **Package** | LQFP-100 |
| **Video Input** | 24-bit parallel RGB, up to 165MHz |
| **Audio Input** | 8-channel I2S (up to 768kHz) or S/PDIF |
| **Documentation** | Excellent ([datasheet](https://www.analog.com/en/products/adv7511.html)) |

### GPIO Requirements

External transmitter requires significantly more pins:

| Signal | Pins |
|--------|------|
| RGB Data (24-bit) | 24 |
| HSYNC, VSYNC, DE | 3 |
| Pixel Clock | 1 |
| I2S (DATA, BCLK, LRCK) | 3 |
| I2C Config | 2 |
| **Total** | ~33 |

**Pros:**
- Guaranteed stable audio (hardware TERC4 encoding)
- Industry-standard approach
- Well-documented ICs

**Cons:**
- Additional BOM cost
- High GPIO pin count
- Requires new PCB design
- More complex routing (parallel RGB timing)

---

## Comparison Summary

| Approach | Complexity | Audio Stability | Pins | Cost | Best For |
|----------|------------|-----------------|------|------|----------|
| **PIO DVI (ikjordan)** | Medium | Proven | ~14 | Free | Testing, production |
| **HSTX + RAW mode** | High | Unknown | 8 | Free | Experimental |
| **MS7210 external** | Low | Guaranteed | ~33 | ~$1.50 | Production (if GPIO available) |
| **ADV7511 external** | Low | Guaranteed | ~33 | ~$10 | High-quality production |

---

## Recommendation for NeoPico-HD

### For Immediate Testing
Use **Option 1 (ikjordan/PicoDVI)**:
- Same wiring as current setup
- Proven audio support
- Can validate HDMI audio with existing hardware

### For Production
Either:
1. **Stick with PIO DVI** if testing proves stable
2. **Add MS7210** if GPIO budget allows and guaranteed stability is required

### RP2350B Advantage
The RP2350B (QFN-80) has 48 GPIOs, making the external transmitter option viable if needed. Standard RP2350A (QFN-60) with 30 GPIOs would be tight.

---

## References

- [ikjordan/PicoDVI](https://github.com/ikjordan/PicoDVI) - PIO DVI with audio
- [hsdaoh-rp2350](https://github.com/steve-m/hsdaoh-rp2350) - HSTX data island encoding
- [pico-examples HSTX](https://github.com/raspberrypi/pico-examples/tree/master/hstx) - Official HSTX examples
- [Adafruit HSTX DVI Adapter](https://www.adafruit.com/product/6055) - Hardware reference
- [MS7210 LCSC](https://www.lcsc.com/product-detail/C2914249.html) - External transmitter
- [ADV7511 Analog Devices](https://www.analog.com/en/products/adv7511.html) - External transmitter
- [RP2350 Datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf) - HSTX chapter (page 1118+)
- [Hackaday HSTX Analysis](https://hackaday.com/2024/08/20/close-up-on-the-rp2350-hstx-peripheral/)
- [DigiKey HSTX Tutorial](https://www.digikey.cz/en/maker/tutorials/2025/what-is-the-rp2350-high-speed-transmit-interface-hstx)
