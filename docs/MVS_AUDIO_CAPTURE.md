# MVS Audio Capture - Technical Findings

This document captures the findings from debugging the YM2610 digital audio capture on Neo Geo MVS hardware.

## Key Breakthrough: MV1C Uses Linear PCM (Not Floating-Point)

**Critical discovery**: The MV1C board does NOT output YM2610 floating-point format externally.

```
YM2610 (floating-point) → NEO-YSA2 (converts) → BU9480F DAC (16-bit linear PCM)
```

The NEO-YSA2 chip internally converts the YM2610's floating-point output to **16-bit linear PCM** for the BU9480F DAC. This means:

1. **Do NOT apply floating-point decode** - data is already linear PCM
2. **Use raw 16-bit signed values directly**: `int16_t sample = (int16_t)(raw & 0xFFFF);`

Reference: cps2_digiav uses `i2s_rx_asrc` with `MODE=1` (right-justified) and NO floating-point decode for Neo Geo.

## Signal Format (Right-Justified)

| Parameter | Value |
|-----------|-------|
| Format | **Right-justified (MODE=1)** - not I2S |
| Data width | 16-bit signed PCM |
| Sample rate | ~55.5 kHz |
| Frame size | 24 bits per channel (8 padding + 16 data) |
| Total frame | 48 BCK cycles per WS period |

### WS Polarity (Critical!)

For right-justified format, the WS polarity is **opposite to I2S**:

| WS Level | Channel |
|----------|---------|
| **WS = 1** | **LEFT** |
| **WS = 0** | **RIGHT** |

Reference: `cps2_digiav/rtl_common/i2s_rx_asrc.v` line 36:
```verilog
localparam LEFT_LEVEL = (MODE == 0) ? 1'b0 : 1'b1;
```

## Signal Characteristics

| Signal | Frequency | Voltage | Notes |
|--------|-----------|---------|-------|
| BCK (Bit Clock) | 2.66 MHz | 5V | ~48x WS frequency |
| WS (Word Select) | 55.5 kHz | 5V | Sample rate |
| DAT (Data) | varies | 5V | Serial audio data |

### Frame Structure

```
One WS period (18 µs @ 55.5 kHz):
├── WS HIGH: Left Channel (24 BCK cycles) ───────────────────┤
│   ├── 8 BCK: Padding (zeros)                               │
│   └── 16 BCK: Audio data (MSB first)                       │
├── WS LOW: Right Channel (24 BCK cycles) ───────────────────┤
│   ├── 8 BCK: Padding (zeros)                               │
│   └── 16 BCK: Audio data (MSB first)                       │
```

## Hardware Setup

### Level Shifting: NOT Required

Testing showed that **removing the level shifter improves dynamic range by ~40%**:

| Setup | Dynamic Range |
|-------|---------------|
| With TXB0108 level shifter | ±12,000 |
| Direct connection | ±17,000 |

The RP2350 GPIO inputs appear to handle the 5V signals adequately for this application.

### GPIO Pin Assignment (Protoboard)

```
Audio Capture:
  GPIO 21: BCK (bit clock)
  GPIO 23: DAT (serial data)
  GPIO 24: WS (word select)

DVI Output:
  GPIO 25-32: DVI signals
```

## PIO Capture Sequence

```
1. Wait for WS = 0 (sync to right channel ending)
2. Wait for WS = 1 (LEFT channel starts)
3. Capture 24 bits on BCK rising edges
4. Push LEFT sample (lower 16 bits are audio)
5. Wait for WS = 0 (RIGHT channel starts)
6. Capture 24 bits on BCK rising edges
7. Push RIGHT sample
8. Loop from step 2
```

## Data Extraction

From the 24-bit captured word:
```c
// Bits [23:16] = Padding (zeros)
// Bits [15:0]  = 16-bit signed PCM audio
int16_t sample = (int16_t)(raw_word & 0xFFFF);
```

## Audio Processing Pipeline

```
PIO Capture → DC Block Filter → FIR Lowpass → SRC (55.5k→48k) → HDMI Audio
```

1. **DC Blocking Filter**: Removes any DC offset
2. **FIR Lowpass Filter**: 16-tap anti-aliasing filter (from cps2_digiav)
3. **Sample Rate Conversion**: Linear interpolation from 55.5kHz to 48kHz

## What NOT to Do

| Wrong Approach | Why It Fails |
|----------------|--------------|
| YM2610 floating-point decode | Data is already linear PCM |
| I2S WS polarity (WS=0 is LEFT) | Right-justified uses opposite polarity |
| 32-bit frame assumption | MVS uses 48-bit frames (24 per channel) |
| Level shifter | Reduces dynamic range |

## References

- `reference/cps2_digiav/board/neogeo/rtl/i2s_upsampler_asrc.v` - Correct MODE=1 usage
- `reference/cps2_digiav/rtl_common/i2s_rx_asrc.v` - WS polarity definition
- `reference/cps2_digiav/board/neogeo/rtl/fir_2ch_audio.v` - FIR filter coefficients
