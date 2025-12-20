# HSTX vs PicoDVI for HDMI Audio

Research conducted 2024-12-20 on alternatives to PicoDVI for HDMI audio output.

## Problem Statement

Current PicoDVI implementation requires significant "gymnastics" for HDMI audio:
- Data islands require precise timing during horizontal blanking
- At 126MHz (240p), CPU can't keep up with TMDS encoding + data island encoding
- Had to switch to 480p @ 252MHz to get reliable audio
- No timer interrupts allowed during video capture (causes trembling)
- Vblank-driven audio buffer filling only

## Current Implementation

Using [ikjordan/PicoDVI](https://github.com/ikjordan/PicoDVI) fork with audio support from:
- [shuichitakano/pico_lib](https://github.com/shuichitakano/pico_lib) - Original HDMI audio (C++)
- mlorenzati - C port of data island encoding
- fruit-bat - Graceful underrun handling

### Current Resource Usage

| Resource | Usage |
|----------|-------|
| PIO0 SM0-2 | DVI TMDS encoding |
| PIO1 SM0-1 | MVS capture |
| PIO2 SM0 | I2S capture |
| Core 1 | DVI output (IRQ-driven) |
| System clock | 252 MHz (required for audio) |

---

## HSTX: A Viable Alternative

### Proof of Concept: hsdaoh-rp2350

[steve-m/hsdaoh-rp2350](https://github.com/steve-m/hsdaoh-rp2350) proves that **HSTX can output HDMI with data islands**.

Steve Markgraf built on Shuichi Takano's TERC4/data island encoding to create a working HSTX-based HDMI implementation for high-speed data acquisition.

Key insight: The HSTX command expander handles both raw (pre-encoded) and TMDS data:

```c
HSTX_CMD_RAW         // Pre-encoded control symbols & data islands
HSTX_CMD_RAW_REPEAT  // Repeat pre-encoded symbols
HSTX_CMD_TMDS        // Hardware TMDS encoding for video pixels
HSTX_CMD_TMDS_REPEAT // Repeat TMDS-encoded pixels
```

### How HSTX Data Islands Work

From `reference/hsdaoh-rp2350/libpicohsdaoh/`:

1. **data_packet.c** implements:
   - TERC4 symbol encoding (10-bit symbols for 4-bit data)
   - BCH parity calculation for error correction
   - Guardband + header + subpacket structure
   - AVI InfoFrame packet creation

2. **picohsdaoh.c** shows HSTX integration:
   - Pre-compute data island during init
   - Insert during vsync using `HSTX_CMD_RAW`
   - DMA feeds command stream to HSTX FIFO
   - No CPU involvement during output

### Data Island Structure

```
| Guardband | Preamble | Header (4 bytes) | Subpackets (4x8 bytes) | Guardband |
|    2 px   |   8 px   |      32 px       |                        |    2 px   |
```

Each element is TERC4-encoded into 10-bit symbols, then packed for HSTX output.

### What's Missing for Audio

The hsdaoh code **removed audio sample packet encoding** (not needed for data acquisition). The original pico_lib has these functions that would need to be ported:

```cpp
// From shuichitakano/pico_lib/dvi/data_packet.cpp
int setAudioSample(const std::array<int16_t, 2> *p, int n, int frameCt);
void setAudioInfoFrame(...);  // Header 0x84, audio metadata
void setAudioClockRegeneration(...);  // CTS/N values for sync
```

---

## Comparison: PicoDVI vs HSTX

| Aspect | PicoDVI (current) | HSTX |
|--------|-------------------|------|
| PIO usage | 3 SMs on PIO0 | **None** |
| TMDS encoding | Software (CPU intensive) | **Hardware** |
| Overclocking | Required (252MHz) | Not required |
| Timing constraints | Complex interrupt coordination | **Deterministic** |
| Data islands | CPU-encoded during blanking | **Pre-computed, DMA-fed** |
| Code complexity | High (C++ templates, timing hacks) | Lower (direct hardware control) |

### HSTX Benefits

1. **Frees all PIOs** - PIO0, PIO1, PIO2 all available for MVS/I2S capture
2. **Hardware TMDS** - Built-in encoder, no CPU cycles for video
3. **Deterministic timing** - HSTX peripheral has consistent output
4. **Cleaner audio path** - Data islands pre-computed, DMA-fed during blanking
5. **No timing fragility** - Can use timer interrupts if needed

### HSTX Constraints

1. **Fixed pins** - GPIO 12-19 only (hardware limitation)
2. **Output only** - Cannot receive, only transmit
3. **RP2350 only** - Not available on RP2040

---

## Pin Conflict Analysis

### Current Pin Assignments

```
GPIO 0     : MVS PCLK
GPIO 1-5   : MVS Green (5 bits)
GPIO 6-10  : MVS Blue (5 bits)
GPIO 11-15 : MVS Red (5 bits)     <-- CONFLICTS with HSTX
GPIO 22    : MVS CSYNC
GPIO 25-32 : DVI output (PicoDVI)
GPIO 36-38 : I2S audio capture
```

### HSTX Fixed Pinout (Pico DVI Sock)

```
GPIO 12-13 : D0+/D0-
GPIO 14-15 : CLK+/CLK-
GPIO 16-17 : D2+/D2-
GPIO 18-19 : D1+/D1-
```

### Proposed New Pin Mapping (RP2350B)

Since RP2350B has 48 GPIOs, we can remap MVS capture:

```
GPIO 0     : MVS PCLK (unchanged)
GPIO 1-5   : MVS Green (unchanged)
GPIO 6-10  : MVS Blue (unchanged)
GPIO 12-19 : HSTX DVI output (NEW)
GPIO 22    : MVS CSYNC (unchanged)
GPIO 36-38 : I2S audio (unchanged)
GPIO 40-44 : MVS Red (MOVED from 11-15)
```

**Advantages:**
- Red channel on Bank 1 (GPIO 32+), isolated from video output noise
- All 3 PIOs free for capture (no DVI PIO usage)
- HSTX handles all DVI output

**Considerations:**
- Requires hardware revision (PCB change)
- Need to verify PIO gpio_base can reach GPIO 40-44

---

## Implementation Path

### Phase 1: Port Data Island Code

1. Copy `data_packet.c/.h` from hsdaoh-rp2350
2. Add audio sample packet encoding from pico_lib
3. Test data island generation independently

### Phase 2: Create HSTX DVI Driver

1. Port `picohsdaoh.c` HSTX initialization
2. Adapt for video output (not data acquisition)
3. Implement scanline DMA similar to PicoDVI
4. Add audio data island insertion during vblank

### Phase 3: Audio Integration

1. Implement Audio Sample Packets (L-PCM, 48kHz, 16-bit)
2. Implement Audio Clock Regeneration (CTS/N for 48kHz)
3. Implement Audio InfoFrame (channel count, sample rate)
4. Test with various HDMI sinks

### Phase 4: Hardware Revision

1. Design new PCB with HSTX-compatible pinout
2. Move MVS Red to GPIO 40-44
3. Route HSTX pins (12-19) to HDMI connector
4. Test signal integrity

---

## References

### Code Repositories

- [hsdaoh-rp2350](https://github.com/steve-m/hsdaoh-rp2350) - HSTX + data islands proof
- [shuichitakano/pico_lib](https://github.com/shuichitakano/pico_lib) - Original HDMI audio (C++)
- [ikjordan/PicoDVI](https://github.com/ikjordan/PicoDVI) - Current audio fork
- [DispHSTX](https://github.com/Panda381/DispHSTX) - Video-only HSTX driver
- [pico-examples dvi_out_hstx_encoder](https://github.com/raspberrypi/pico-examples/tree/master/hstx/dvi_out_hstx_encoder)

### Documentation

- [RP2350 HSTX Interface (CNX Software)](https://www.cnx-software.com/2024/08/15/raspberry-pi-rp2350-hstx-high-speed-serial-transmit-interface/)
- [Close-up on HSTX (Hackaday)](https://hackaday.com/2024/08/20/close-up-on-the-rp2350-hstx-peripheral/)
- [Raspberry Pi Forums: DVI/HDMI audio](https://forums.raspberrypi.com/viewtopic.php?t=348724)

### Hardware

- [Adafruit HSTX to DVI Adapter](https://www.adafruit.com/product/6055)
- [Pico DVI Sock](https://github.com/Wren6991/Pico-DVI-Sock)

---

## Conclusion

HSTX is a viable and potentially superior alternative to PicoDVI for HDMI audio output. The key advantages are:

1. **Hardware TMDS encoding** eliminates CPU overhead
2. **All PIOs freed** for capture tasks
3. **Pre-computed data islands** reduce timing complexity
4. **Proven implementation** exists in hsdaoh-rp2350

The main challenges are:
1. Fixed GPIO 12-19 pinout requires hardware revision
2. Audio packet encoding needs to be ported from pico_lib
3. New DVI driver needs to be written

**Recommendation:** Pursue HSTX-based implementation for next hardware revision.
