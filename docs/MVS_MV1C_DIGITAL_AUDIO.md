# Neo Geo MVS MV1C Digital Audio Specification

Technical specification for the digital audio signals on the Neo Geo MVS MV1C arcade board.

## Hardware Overview

The MV1C uses the **NEO-YSA2** chip (integrates YM2610 + Z80 + RAM) which outputs digital audio to a **BU9480F** DAC via a serial interface.

```
YM2610 (floating-point) → NEO-YSA2 (converts) → BU9480F DAC
                                               ↑
                                         Tap point here
                                         (16-bit linear PCM)
```

**Critical**: The NEO-YSA2 internally converts YM2610's floating-point format to **16-bit linear PCM**. No floating-point decoding is required when tapping the digital output.

## Signal Format

| Parameter | Value |
|-----------|-------|
| Format | **Right-justified** (NOT I2S) |
| Data width | 16-bit signed PCM (2's complement) |
| Sample rate | ~55.5 kHz (8 MHz / 144) |
| Frame size | 24 BCK cycles per channel |
| Total frame | 48 BCK cycles per WS period |

### Signal Characteristics

| Signal | Frequency | Voltage | Description |
|--------|-----------|---------|-------------|
| BCK | 2.66 MHz | 5V | Bit clock (~48x WS) |
| WS | 55.5 kHz | 5V | Word select (sample rate) |
| DAT | varies | 5V | Serial audio data, MSB first |

## WS Polarity (Critical!)

Right-justified format uses **opposite polarity** from standard I2S:

| WS Level | Channel |
|----------|---------|
| **WS = HIGH** | **LEFT** |
| **WS = LOW** | **RIGHT** |

This is the opposite of standard I2S where WS LOW = LEFT.

## Frame Structure

```
One WS period (~18 µs @ 55.5 kHz):

WS HIGH (Left Channel) ─────────────────────────────────────────
├── 8 BCK: Padding (zeros/don't care)
└── 16 BCK: Audio data (MSB first, bits 15→0)

WS LOW (Right Channel) ─────────────────────────────────────────
├── 8 BCK: Padding (zeros/don't care)
└── 16 BCK: Audio data (MSB first, bits 15→0)
```

### Bit Timing

```
BCK:    ─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─  ...
         └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘

DAT:    ─X───X───X───X───X───X───X───X───X─  ...
          D23  D22  D21  ...        D1   D0

WS:     ─────────────────────────┐
                                 └─────────  (transition marks channel boundary)
```

- Data sampled on BCK **rising edge**
- WS transitions indicate channel boundaries
- First 8 bits (D23-D16) are padding
- Last 16 bits (D15-D0) are audio data

## Data Extraction

From the 24-bit captured word:

```c
// Bits [23:16] = Padding (zeros)
// Bits [15:0]  = 16-bit signed PCM audio
int16_t sample = (int16_t)(raw_24bit & 0xFFFF);
```

The 16-bit value is signed 2's complement, ready for use without any decoding.

## Sample Rate

| Parameter | Value |
|-----------|-------|
| Master clock | 8 MHz |
| Divisor | 144 |
| Sample rate | 55,555 Hz (~55.5 kHz) |
| Period | 18.0 µs |

For HDMI output at 48 kHz, sample rate conversion is required (55.5:48 ≈ 1.157:1 decimation).

## Tap Points (MV1C Board)

Signals are tapped near IC4 (BU9480F), on the NEO-YSA2 side of the resistors:

| Signal | Location | Description |
|--------|----------|-------------|
| WS | R90 | Word select (LRCK) |
| DAT | R91 | Serial data |
| BCK | R92 | Bit clock |
| GND | - | Common ground (required!) |

**Important**: A common ground connection between the capture device and MVS board is essential for signal integrity.

## Level Shifting

The MVS outputs 5V logic levels. Testing shows:

| Approach | Result |
|----------|--------|
| TXB0108 level shifter | Reduced dynamic range (~±12,000) |
| Direct connection to 3.3V GPIO | Full dynamic range (~±17,000) |

Many 3.3V microcontrollers can tolerate 5V inputs on their GPIO pins. Check your device's specifications.

## Common Mistakes

| Wrong Approach | Why It Fails |
|----------------|--------------|
| YM2610 floating-point decode | Data is already linear PCM after NEO-YSA2 |
| I2S WS polarity (WS=0 is LEFT) | Right-justified uses opposite polarity |
| 32-bit frame assumption | MV1C uses 48-bit frames (24 per channel) |
| Using 49 kHz sample rate | Actual rate is 55.5 kHz |
| Skipping ground connection | Causes signal integrity issues |

## Comparison: MV1C vs Earlier MVS Boards

| Board Type | Audio Chip | DAC | Digital Format |
|------------|------------|-----|----------------|
| Early MVS | YM2610 | YM3016 | Floating-point (10-bit mantissa + 3-bit exponent) |
| **MV1C** | **NEO-YSA2** | **BU9480F** | **Linear 16-bit PCM** |
| CDZ | NEO-YSA2 | BU9480F | Linear 16-bit PCM |

## References

- [NEO-YSA2 - NeoGeo Dev Wiki](https://wiki.neogeodev.org/index.php?title=NEO-YSA2)
- [BU9480F Datasheet](https://www.alldatasheet.com/datasheet-pdf/pdf/36378/ROHM/BU9480F.html)
- [YM3016 - NeoGeo Dev Wiki](https://wiki.neogeodev.org/index.php?title=YM3016)
- cps2_digiav project (rtl_common/i2s_rx_asrc.v)
