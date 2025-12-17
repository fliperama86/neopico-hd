# CPS2_digiav NeoGeo MVS Audio Capture Analysis

Analysis of digital audio capture in the cps2_digiav project for NeoGeo MVS (MV1C).

## Hardware Overview

The **MV1C** uses the **NEO-YSA2** chip (integrates YM2610 + Z80 + RAM) which outputs digital audio to a **BU9480F** DAC.

**Key point**: Unlike earlier boards using the YM3016 (Yamaha floating-point DAC), the MV1C outputs **standard 16-bit linear PCM** - no floating-point decoding required.

## Tap Points (MV1C)

From `board/neogeo/doc/mv1c_hookup_points.txt`:

| Signal | Location | Description |
|--------|----------|-------------|
| I2S_WS | R90 | Word Select (LRCK) |
| I2S_DAT | R91 | Serial Data |
| I2S_BCK | R92 | Bit Clock |

Tapped near IC4 (BU9480F), between NEO-YSA2 and DAC.

## Input Signal Format

| Parameter | Value |
|-----------|-------|
| Bit Depth | 16-bit |
| Format | Right-justified, MSB first, 2's complement |
| Sample Rate | ~55.6 kHz |
| WS Polarity | HIGH = Left, LOW = Right |

## Processing Pipeline

```
MV1C Board      i2s_rx_asrc       fir_2ch_audio      i2s_tx_asrc       HDMI TX
(NEO-YSA2)  -->  (MODE=1)     -->  (16x decim)   -->  (2x decim)   -->  Output
16-bit serial    16-bit parallel   24-bit            24-bit I2S        48kHz
~55.6kHz                           ~96kHz
```

### Stage 1: Deserialization (`i2s_rx_asrc`)

- MODE=1 (right-justified)
- WS HIGH = left channel capture
- Output: 16-bit parallel L/R samples

### Stage 2: FIR Decimation (`fir_2ch_audio`)

Altera FIR II IP with 16x decimation:

| Parameter | Value |
|-----------|-------|
| Decimation | 16x |
| Input | 16-bit |
| Output | 24-bit |
| Filter | 16-tap symmetric lowpass |
| MCLK | 24.576 MHz |

### Stage 3: Output (`i2s_tx_asrc`)

- Additional 2x downsampling (`downsample_2x=1`)
- 64 BCKs per frame
- Final output: **24-bit @ 48kHz**

**Total decimation**: 16 x 2 = 32x

## Format Clarification

| Board Type | DAC | Digital Format |
|------------|-----|----------------|
| Early MVS | YM3016 | Floating-point (10-bit mantissa + 3-bit exponent) |
| MV1C, CDZ | BU9480F | **Linear 16-bit PCM** |

The `ym_rx_asrc.v` floating-point decoder exists in cps2_digiav but is **NOT used** for NeoGeo - only for CPS2 boards.

## References

- [NEO-YSA2 - NeoGeo Dev Wiki](https://wiki.neogeodev.org/index.php?title=NEO-YSA2)
- [YM3016 - NeoGeo Dev Wiki](https://wiki.neogeodev.org/index.php?title=YM3016)
- [NeoGeoHDMI Notes](https://github.com/charcole/NeoGeoHDMI/blob/master/Notes.md)
- [BU9480F Datasheet](https://www.alldatasheet.com/datasheet-pdf/pdf/36378/ROHM/BU9480F.html)
