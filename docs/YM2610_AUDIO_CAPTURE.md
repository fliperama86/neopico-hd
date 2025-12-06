# YM2610 Audio Capture - Key Findings

## Overview

This document captures findings from reverse-engineering the YM2610 digital audio output on the Neo Geo MVS for capture via RP2350B.

## Signal Specifications

### Pin Assignments (on Pico)

| GPIO | Signal | Description |
|------|--------|-------------|
| 3    | DAT    | Serial audio data |
| 9    | WS     | Word select (L/R channel indicator) |
| 23   | BCK    | Bit clock |

### Measured Frequencies

| Signal | Frequency | Notes |
|--------|-----------|-------|
| BCK    | 2.667 MHz | Bit clock |
| WS     | 55.5 kHz  | Sample rate (per channel) |
| BCK/WS | 48        | Bits per stereo sample |

### Frame Format

```
Total bits per stereo frame: 48
Bits per channel: 24
Sample rate: 55,500 Hz (55.5 kHz)
```

**Important:** This is NOT standard I2S. The YM2610 uses 24 bits per channel with 48 BCK cycles per WS period.

## Protocol Details

### WS Polarity

- **WS Rising Edge** = Start of LEFT channel
- WS High = Left channel data
- WS Low = Right channel data

This is opposite to standard I2S where WS low typically indicates left channel.

### Clock Edge

- Data is sampled on **BCK Rising Edge**
- Tested: Falling edge produced worse audio

### Bit Extraction

The 24-bit samples are captured into 32-bit words. To convert to 16-bit audio:

```c
// Take upper 16 bits of 24-bit sample (bits 23-8)
int16_t sample = (int16_t)(raw_sample >> 8);
```

## Hardware Considerations

### Noise Filtering

The WS signal is susceptible to crosstalk from BCK due to proximity on the PCB. Solutions:

1. **Schmitt trigger (hysteresis)** on all input pins:
   ```c
   gpio_set_input_hysteresis_enabled(pin, true);
   ```

2. **Separate PIO clock rates** - Use slower clock for WS capture to filter BCK noise:
   ```c
   // For frequency measurement, use 500kHz for WS vs 10MHz for BCK
   ```

3. **Shared ground** between Pico and MVS is essential for clean signals

### PIO Constraints (RP2350B)

- PIO0 and PIO1: GPIO 0-31 only
- PIO2: GPIO 16-47
- For GPIO 23 (BCK), must use PIO0 or PIO1

## Capture Implementation

### PIO Program

```pio
.program ym2610_capture
.wrap_target
wait_frame:
    wait 0 gpio 9       ; Wait for WS low
    wait 1 gpio 9       ; Wait for WS rising edge = left channel start

    set x, 23           ; 24 bits
left_loop:
    wait 0 gpio 23      ; BCK low
    wait 1 gpio 23      ; BCK rising edge
    in pins, 1          ; Sample DAT
    jmp x-- left_loop
    push noblock

    set x, 23           ; 24 bits
right_loop:
    wait 0 gpio 23
    wait 1 gpio 23
    in pins, 1
    jmp x-- right_loop
    push noblock
.wrap
```

### DMA Configuration

- Double-buffered 32-bit transfers
- DREQ paced by PIO RX FIFO
- ~1024 stereo samples per buffer

## Audio Quality Analysis

### Typical Measurements

| Metric | Value | Notes |
|--------|-------|-------|
| Peak Level | -0.9 dBFS | Near full scale |
| RMS Level | -25 dBFS | Typical game audio |
| DC Offset | ~0.9% | Acceptable |
| Clipping | Rare | Occasional 1-sample clips |

### Known Issues

1. **Startup spike** - First few samples may have large amplitude spike. Likely initialization artifact.

2. **DC offset** - ~0.9% offset present. Could be removed in firmware if needed.

3. **Silent periods** - KOF98 attract mode has silent moments; not a capture issue.

## Test Infrastructure

### Automated Testing

```bash
# Run audio capture test
./scripts/test_audio.sh

# With rebuild and flash
./scripts/test_audio.sh --rebuild --flash
```

### Output Files

| File | Description |
|------|-------------|
| `viewer/test_*.wav` | Captured audio |
| `viewer/test_*.png` | Analysis plot (waveform, spectrum, histogram) |

### Analysis Report

The test script outputs:
- Duration, sample rate, sample count
- Peak/RMS levels in dBFS
- DC offset percentage
- Clipping detection
- Dominant frequencies (FFT)
- Quality score (0-100)

## References

- YM2610 Datasheet
- Neo Geo MVS schematics
- YM3016 DAC specifications (for analog output path)

## Future Work

1. Decode YM2610 floating-point format (if needed for better quality)
2. Remove DC offset in firmware
3. Investigate startup spike
4. Integration with DVI output for HDMI audio
