# Audio Capture for NeoPico-HD

## Overview

The Neo Geo MVS uses a **Yamaha YM2610 (OPNB)** sound chip. Unlike the digital RGB video bus, the YM2610 outputs **analog audio only** through external DACs and amplifiers. To capture audio digitally, an external ADC is required.

## YM2610 Sound Architecture

| Channel Type | Count | Description |
|--------------|-------|-------------|
| FM synthesis | 4 | 4-operator FM channels |
| SSG (PSG) | 3 | Square wave channels |
| Noise | 1 | Noise generator |
| ADPCM-A | 6 | 12-bit samples @ 18.5 kHz fixed |
| ADPCM-B | 1 | 16-bit samples @ 1.8-55.5 kHz variable |

The FM and ADPCM outputs go through a **YM3016 stereo DAC**, then through analog mixing and amplification before reaching the audio output jacks.

## cps2_digiav Approach

The cps2_digiav project uses a dedicated **analog-to-digital converter (ADC) daughter board** to digitize the MVS audio output.

### Hardware: neogeo_aadc PCB

Located in `reference/cps2_digiav/pcb_neogeo_aadc/`

**Components:**
- **WM8782** - Wolfson 24-bit stereo ADC
  - Analog inputs: AINL, AINR (from MVS audio output)
  - Digital outputs: I2S (DATA, WS, BCK)
  - Requires master clock (MCLK) input
- **TLV70033** - 3.3V LDO regulator (from 5V)
- **74LVC1G17** - Schmitt trigger buffer for MCLK
- Input RC filtering (5kΩ + 10µF per channel)

**Connections to MVS:**
```
MVS Audio L ──[5kΩ]──[10µF]──→ AINL
MVS Audio R ──[5kΩ]──[10µF]──→ AINR
MVS 5V ─────────────────────→ 5V
MVS GND ────────────────────→ GND
```

**Connections to FPGA/Pico:**
```
MCLK ←── Master clock output from FPGA/Pico
DATA ──→ I2S serial data
WS   ──→ Word select (LRCLK) - indicates L/R channel
BCK  ──→ Bit clock
```

### Software Pipeline

```
┌─────────────────────────────────────────────────────────┐
│                    cps2_digiav Pipeline                 │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  MVS Analog Audio                                       │
│         ↓                                               │
│  [WM8782 ADC] ← MCLK from FPGA                         │
│         ↓                                               │
│  I2S (16-bit stereo, native sample rate)               │
│         ↓                                               │
│  i2s_rx_asrc.v                                         │
│    - Deserializes I2S to parallel samples              │
│    - Detects WS transitions for L/R separation         │
│    - MODE=0: Standard I2S format                       │
│    - MODE=1: Right-justified format                    │
│         ↓                                               │
│  fir_2ch_audio (FIR interpolation filter)              │
│    - Upsamples from ~18.5kHz to 48kHz                  │
│    - 2-channel muxed design                            │
│         ↓                                               │
│  i2s_tx_asrc.v                                         │
│    - Serializes to HDMI I2S format                     │
│    - 48kHz output for HDMI compliance                  │
│         ↓                                               │
│  HDMI audio stream                                      │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### Key Implementation Details

From `rtl_common/i2s_rx_asrc.v`:
- 16-bit samples per channel
- Configurable MODE: 0=I2S standard, 1=right-justified
- MCLK_DIVIDER=16 for output timing
- Double-buffered sample capture
- WS edge detection for L/R channel identification

## Pico Implementation Options

### Option 1: External I2S ADC (Recommended)

Use a module like PCM1802, WM8782, or INMP441.

**GPIO Assignment:**
| GPIO | Function | Notes |
|------|----------|-------|
| 23 | I2S_DATA | Serial audio data |
| 24 | I2S_WS | Word select (LRCLK) |
| 25 | I2S_BCK | Bit clock |
| 29 | MCLK_OUT | Master clock to ADC (PWM) |

**PIO I2S Receiver:**
```pio
; Simple I2S receiver - captures 16-bit stereo
; Autopush at 32 bits = one L+R sample pair

.program i2s_rx
.side_set 1 opt

    wait 1 pin 1        ; Wait for WS high (right channel start)
    wait 0 pin 1        ; Wait for WS low (left channel start)
    set x, 15           ; 16 bits to receive

bitloop:
    wait 0 pin 2        ; Wait for BCK low
    wait 1 pin 2        ; Wait for BCK high
    in pins, 1          ; Sample DATA pin
    jmp x-- bitloop     ; Loop for 16 bits

    ; Repeat for right channel (16 more bits)
    set x, 15
bitloop_r:
    wait 0 pin 2
    wait 1 pin 2
    in pins, 1
    jmp x-- bitloop_r
    ; Autopush happens here (32 bits)
```

**Advantages:**
- 16-bit resolution (same as cps2_digiav)
- Clean digital signal path
- PicoDVI supports HDMI audio embedding
- Low CPU overhead (PIO + DMA)

**Disadvantages:**
- Requires additional hardware (~$5-10)
- More wiring

### Option 2: Pico's Built-in ADC

Use the RP2350's 12-bit ADC directly.

**GPIO Constraints:**
- GPIO 26-27: Used by DVI clock (not available)
- GPIO 28: Used by MVS PCLK (not available)
- GPIO 29: ADC3 - **available for mono audio**

**Implementation:**
```c
// Sample at 48kHz using timer interrupt
#define AUDIO_SAMPLE_RATE 48000

void audio_timer_callback(void) {
    uint16_t sample = adc_read();  // 12-bit value
    audio_buffer[write_idx++] = sample << 4;  // Expand to 16-bit
}
```

**Advantages:**
- No additional hardware
- Simple implementation

**Disadvantages:**
- Mono only (single ADC pin available)
- 12-bit resolution (lower quality)
- CPU overhead for sampling

### Option 3: Different DVI Pinout

Reassign DVI pins to free up GPIO 26-27 for stereo ADC.

This would require changes to:
- `neopico_dvi_cfg` in main.c
- Physical wiring

Not recommended unless stereo ADC is critical and I2S ADC is unavailable.

## Audio Format Specifications

### HDMI Audio Requirements

| Parameter | Value |
|-----------|-------|
| Sample rate | 32kHz, 44.1kHz, or 48kHz |
| Bit depth | 16-bit or 24-bit |
| Channels | 2 (stereo) |
| Format | IEC 60958 (S/PDIF-like) |

PicoDVI supports 48kHz stereo audio embedding.

### MVS Native Audio

| Parameter | Value |
|-----------|-------|
| Sample rate | ~18.5 kHz (ADPCM-A) |
| Bit depth | 12-16 bit (internal) |
| Channels | 2 (stereo) |

The cps2_digiav uses a FIR filter to upsample from ~18.5kHz to 48kHz for HDMI compliance.

## Memory Budget

```
Audio ring buffer (1 second @ 48kHz stereo):
  48000 samples × 2 channels × 2 bytes = 192 KB

Practical buffer (1 frame @ 60fps):
  48000 / 60 = 800 samples
  800 × 2 × 2 = 3.2 KB per frame
  Double buffer: 6.4 KB

Current video framebuffer: ~150 KB
Audio overhead: < 5% additional RAM
```

## References

- `reference/cps2_digiav/pcb_neogeo_aadc/` - ADC board schematic
- `reference/cps2_digiav/rtl_common/i2s_rx_asrc.v` - I2S receiver
- `reference/cps2_digiav/rtl_common/i2s_tx_asrc.v` - I2S transmitter
- `reference/cps2_digiav/board/neogeo/rtl/i2s_upsampler_asrc.v` - Upsampler
- [YM2610 - NeoGeo Dev Wiki](https://wiki.neogeodev.org/index.php?title=YM2610)
- [PicoDVI Audio Support](https://github.com/Wren6991/PicoDVI)
