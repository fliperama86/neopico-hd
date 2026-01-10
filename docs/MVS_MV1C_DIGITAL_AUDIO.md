# Neo Geo MVS MV1C Digital Audio Specification

Technical specification for the digital audio signals on the Neo Geo MVS MV1C arcade board.

## Signal Format

| Parameter   | Value                                  |
| ----------- | -------------------------------------- |
| Format      | **Right-justified** (NOT standard I2S) |
| Data width  | 16-bit signed PCM (2's complement)     |
| Sample rate | ~55,555 Hz (8 MHz / 144)               |
| Frame size  | 24 BCK cycles per channel              |

### WS Polarity (Critical!)

The MV1C uses opposite polarity from standard I2S:

- **WS HIGH** = LEFT channel
- **WS LOW** = RIGHT channel

## Capture Strategy (NeoPico-HD)

### 1. PIO State Machine

A dedicated PIO state machine (on PIO2) captures the serial stream.

- **Clock Edge**: Samples DAT on the **BCK Rising Edge**.
- **Frame Sync**: Waits for a falling edge on WS to align with the Right channel start.
- **Word Extraction**: Captures 24 bits and pushes them to the FIFO.

### 2. DMA Ring Buffer

A hardware DMA channel drains the PIO FIFO into a 16KB circular buffer in RAM. This provides enough headroom (~2 frames) to allow for "bursty" CPU processing without losing audio data.

### 3. Pipeline Processing

The **Core 1** main loop polls the capture ring and runs the following stages:

- **DC Filter**: Removes DC offset from the MV1C digital source.
- **Lowpass Filter**: Anti-aliasing to ensure a clean sound.
- **SRC (Sample Rate Conversion)**: High-quality decimation from 55.5kHz to 48kHz for HDMI standard compatibility.

Processed samples are then TERC4 encoded and injected into the HDMI stream as Data Islands.

## Clock Synchronization & Drift Control

A critical challenge is synchronizing the **asynchronous** input source (MVS, ~55.555kHz) with the fixed output clock (HDMI, locked to 25.2MHz pixel clock). Even a tiny drift (e.g., 20ppm) results in buffer overflow or underrun every few minutes.

### The Solution: Closed-Loop Feedback

We implement a feedback control loop in `audio_subsystem.c` to dynamically adjust the Sample Rate Converter (SRC) ratio:

1.  **Monitor**: The fill level of the HDMI Data Island queue (`hstx_di_queue`) is sampled every 30 frames (~0.5s).
2.  **Target**: We aim to keep the buffer 50% full (128 packets).
3.  **Adjust**:
    - If buffer > 60%: Increase SRC input rate setting (tells SRC "input is fast", causing it to drop slightly more samples).
    - If buffer < 35%: Decrease SRC input rate setting (tells SRC "input is slow", causing it to emit slightly more samples).
4.  **Deadband**: No changes are made if the level is within the 35%-60% range, ensuring stability.

This mechanism effectively "locks" the audio production rate to the HDMI consumption rate, compensating for thermal drift and clock inaccuracies without audible pitch shifting or glitches.

## Implementation Status (Verified)

- **Capture Rate**: Verified at 55,553 Hz (exact match to MVS).
- **Quality**: Verified crystal clear 48kHz stereo output on HDMI.
- **Stability**: Zero audio buffer overflows or underruns observed during testing (fixed 42-second periodic drift issue).

## Tap Points (MV1C Board)

| Signal  | Location        |
| ------- | --------------- |
| I2S WS  | R90 (or GPIO 1) |
| I2S DAT | R91 (or GPIO 0) |
| I2S BCK | R92 (or GPIO 2) |

**Mandatory**: Use a solid shared ground between the MVS and the Pico board. Digital audio is extremely sensitive to ground potential differences.