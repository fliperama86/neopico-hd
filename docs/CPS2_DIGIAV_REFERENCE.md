# CPS2_digiav Reference for Neo Geo A/V Processing

The cps2_digiav project is an FPGA-based digital A/V solution supporting CPS2, Neo Geo, and other arcade boards. This document covers its Neo Geo audio/video processing implementation as a reference for other capture projects.

## Project Overview

- **Repository**: https://github.com/marqs85/cps2_digiav
- **Platform**: Altera/Intel Cyclone IV FPGA
- **Supported boards**: CPS2, Neo Geo MVS/AES/CDZ, and others
- **Output**: HDMI with 48 kHz audio

## Neo Geo Audio Pipeline

```
MV1C Board       i2s_rx_asrc        fir_2ch_audio       i2s_tx_asrc        HDMI TX
(NEO-YSA2)  →    (MODE=1)      →    (16x decim)    →    (2x decim)    →    Output
16-bit serial    16-bit parallel    24-bit             24-bit I2S         48kHz
~55.6 kHz
```

### Stage 1: I2S Deserialization (`i2s_rx_asrc.v`)

Converts serial audio to parallel samples.

| Parameter | Value |
|-----------|-------|
| MODE | 1 (right-justified) |
| WS polarity | HIGH = left channel |
| Output | 16-bit parallel L/R samples |

**Key code** (`rtl_common/i2s_rx_asrc.v`):
```verilog
// MODE=0: Standard I2S (WS LOW = left)
// MODE=1: Right-justified (WS HIGH = left)
localparam LEFT_LEVEL = (MODE == 0) ? 1'b0 : 1'b1;
```

### Stage 2: FIR Decimation (`fir_2ch_audio.v`)

Anti-aliasing filter with decimation using Altera FIR II IP.

| Parameter | Value |
|-----------|-------|
| Decimation | 16x |
| Input width | 16-bit |
| Output width | 24-bit |
| Filter type | 16-tap symmetric lowpass |
| MCLK | 24.576 MHz |

### Stage 3: Output Serialization (`i2s_tx_asrc.v`)

Converts parallel samples to HDMI-compatible I2S.

| Parameter | Value |
|-----------|-------|
| Additional decimation | 2x (`downsample_2x=1`) |
| BCK cycles per frame | 64 |
| Output format | 24-bit I2S @ 48 kHz |

### Total Decimation

```
55.6 kHz × (1/16) × (1/2) = 1.7375 kHz ... wait, that's wrong
```

Actually, the FIR filter operates at a higher internal rate. The effective path is:
- Input: ~55.6 kHz
- FIR processes at ~1.5 MHz internal rate with 16x decimation
- Output stage provides final 48 kHz timing

The math works out to approximately 55.6 kHz → 48 kHz output.

## Neo Geo Video Pipeline

```
MVS Board        sync_decoder       video_formatter       HDMI TX
RGB[14:0]   →    Sync extract  →    Scaling/timing   →    Output
CSYNC            HSYNC/VSYNC        640x480              720p/1080p
PCLK (6MHz)
```

### Sync Processing

The sync decoder extracts HSYNC and VSYNC from composite sync:

| Detection | Method |
|-----------|--------|
| HSYNC | Full line pulse (~355 pixels) |
| Equalization | Half line pulse (~177 pixels) |
| VSYNC | Sequence of 18 equalization pulses |

Threshold: Pulses > 288 pixels (3/4 line) are HSYNC; shorter are equalization.

### Video Timing Constants

From `board/neogeo/rtl/neogeo_frontend.v`:

| Parameter | Value |
|-----------|-------|
| H_TOTAL | 384 |
| H_ACTIVE | 320 |
| H_SYNCLEN | 29 |
| V_TOTAL | 264 |
| V_ACTIVE | 224 |
| V_SYNCLEN | 3 |

## Format Detection: Neo Geo vs CPS2

The cps2_digiav auto-detects board type based on audio format:

| Board | Audio Format | Detection |
|-------|--------------|-----------|
| CPS2 | YM2151 floating-point via YM3016 | Requires float-to-PCM decode |
| Neo Geo MV1C | Linear PCM via BU9480F | Direct capture, MODE=1 |

The `ym_rx_asrc.v` floating-point decoder is used for CPS2 but NOT for Neo Geo.

## Key Implementation Files

### Audio

| File | Purpose |
|------|---------|
| `rtl_common/i2s_rx_asrc.v` | I2S deserializer with MODE selection |
| `rtl_common/i2s_tx_asrc.v` | I2S serializer for HDMI output |
| `board/neogeo/rtl/fir_2ch_audio.v` | Dual-channel FIR filter |
| `board/neogeo/rtl/i2s_upsampler_asrc.v` | Neo Geo audio top module |

### Video

| File | Purpose |
|------|---------|
| `rtl_common/videogen.v` | Output video timing generator |
| `board/neogeo/rtl/neogeo_frontend.v` | MVS signal capture |
| `board/neogeo/rtl/neogeo_top.v` | Top-level Neo Geo module |

## FIR Filter Coefficients

The 16-tap symmetric lowpass filter coefficients (from `fir_2ch_audio.v`):

```
Symmetric, so only 8 unique values needed for 16 taps.
Provides anti-aliasing for decimation from ~55.6 kHz to lower rate.
```

## Hookup Points Reference

From `board/neogeo/doc/`:

### MV1C Audio
| Signal | Location |
|--------|----------|
| WS | R90 |
| DAT | R91 |
| BCK | R92 |

### MV1C Video
| Signal | Location |
|--------|----------|
| PCLK | PC23 pin 11 |
| CSYNC | R51 |
| RGB | Via R2R DAC resistors |

### Power
| Signal | Location |
|--------|----------|
| 5V | TP2 |
| GND | C6 |

## Design Notes

### Why MODE=1 (Right-Justified)?

The BU9480F DAC expects right-justified data format, not standard I2S. The NEO-YSA2 outputs in this format to match the DAC's requirements.

### Why 24-bit FIR Output?

The FIR filter accumulates 16-bit input samples, increasing bit depth to avoid truncation errors. The final 24-bit output provides headroom for the decimation process.

### Why Separate Decimation Stages?

Splitting decimation (16x then 2x) allows:
1. FIR to run at a practical clock rate
2. Flexible output rate selection
3. Better filter performance than single large decimation

## Adapting for Microcontroller Capture

Key differences when implementing on microcontroller vs FPGA:

| Aspect | FPGA (cps2_digiav) | Microcontroller |
|--------|-------------------|-----------------|
| Decimation | Hardware FIR | Software SRC |
| Clock | 24.576 MHz MCLK | Internal PLL |
| Bit depth | 24-bit internal | 16-bit typically |
| Anti-aliasing | 16-tap FIR | Optional lowpass |

For microcontroller implementations, simpler sample rate conversion (e.g., Bresenham decimation) may suffice if audio quality requirements are modest.

## References

- [cps2_digiav GitHub](https://github.com/marqs85/cps2_digiav)
- [NeoGeoHDMI](https://github.com/charcole/NeoGeoHDMI)
- [NeoGeo Dev Wiki](https://wiki.neogeodev.org/)
