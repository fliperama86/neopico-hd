# NeoPico-HD System Architecture

NeoPico-HD is a specialized firmware that transforms the RP2350 microcontroller into a real-time, digital-to-digital capture and upscaling bridge for the Neo Geo MVS.

## 1. High-Level Overview

The system bridges two asynchronous clock domains:
1.  **Input Domain**: The Neo Geo MVS (Variable/Drifting clock ~6 MHz pixel, ~55.5 kHz audio).
2.  **Output Domain**: Standard HDMI/DVI (Fixed clock 25.2 MHz pixel, 48 kHz audio).

Unlike FPGA solutions that "slave" the output clock to the input, NeoPico-HD uses the RP2350's raw horsepower to **resample** the data in real-time, maintaining a standards-compliant HDMI output signal regardless of the input source's behavior.

## 2. Core Responsibilities

The workload is strictly partitioned between the two cores to ensure deterministic timing for critical tasks.

### Core 0: Capture & Conversion (The "Input" Core)
*   **Video Capture**: Uses PIO1 (Bank 1 GPIOs) to sample the 15-bit RGB bus + Control signals at exactly 6 MHz.
*   **Synchronization**: Re-synchronizes the PIO state machine on every single CSYNC falling edge to prevent horizontal drift.
*   **Pixel Processing**:
    *   Detects `SHADOW` and `DARK` bits.
    *   Default build uses a pre-computed 32K LUT for corrected RGB555 -> RGB565 conversion.
    *   Optional mode (`NEOPICO_ENABLE_DARK_SHADOW=ON`) uses a 64K LUT indexed by RGB555+SHADOW.
    *   In optional mode, SHADOW uses legacy main-branch dimming math (halve channels, then apply fixed dark offset); DARK is still captured for diagnostics but is not part of LUT indexing.
    *   Converts raw MVS data into standard RGB565.
*   **Frame Management**: Writes to a ping-pong buffer in RAM.

### Core 1: Output & Audio (The "Output" Core)
*   **Video Output**: Drives the HSTX peripheral to generate 640x480p @ 60Hz DVI/HDMI signals.
*   **Scanline Doubling**: Reads the 320x240 capture buffer and performs line doubling on-the-fly during DMA interrupts.
*   **OSD Overlay**: Injects the On-Screen Display menu pixels during the scanline generation phase.
*   **Audio Pipeline**:
    *   Captures I2S audio via PIO2 (Bank 0).
    *   Runs the **Closed-Loop ASRC** (Asynchronous Sample Rate Converter) to downsample ~55.5kHz to 48kHz.
    *   Encodes audio into TERC4 Data Islands and injects them into the HDMI blanking intervals.

## 3. The Audio Sync Solution (Closed-Loop ASRC)

One of the project's key innovations is handling the asynchronous audio without an FPGA.

### The Problem
The MVS audio clock drifts relative to the RP2350's crystal. A fixed resampling ratio eventually leads to buffer underrun (silence) or overflow (glitches) every ~40 seconds.

### The Solution
We implemented a **Proportional Control Loop**:
1.  **Measure**: The system monitors the fill level of the HDMI Data Island Ring Buffer (`hstx_di_queue`).
2.  **Feedback**:
    *   If buffer > 60% full -> The SRC is "producing too fast". We effectively increase the input rate parameter, causing the SRC to drop more samples to catch up.
    *   If buffer < 35% full -> The SRC is "producing too slow". We decrease the input rate parameter, causing the SRC to emit more samples.
3.  **Result**: The audio buffer hovers around 50%, essentially "locking" the audio production rate to the video consumption rate.

## 4. Memory Map

| Region | Size | Usage |
| :--- | :--- | :--- |
| **SRAM_STRIPED** | 64-128 KB | **Video LUT** (64 KB default RGB LUT, 128 KB with DARK/SHADOW mode) |
| **SRAM** | 150 KB | **Framebuffer** (320x240 RGB565) |
| **SRAM** | 16 KB | **Audio DMA Buffer** (Raw I2S capture) |
| **SCRATCH_X** | 4 KB | **Core 1 ISRs** (HSTX/Audio critical code) |
| **SCRATCH_Y** | 4 KB | **Core 0 ISRs** (Capture critical code) |

## 5. Hardware Interfaces

*   **Bank 0 (GPIO 0-29)**: Standard I/O. Used for I2S Audio (PIO2) and HSTX Output.
*   **Bank 1 (GPIO 25-47)**: High-speed capture. Used for the MVS 24-bit video bus.
    *   *Note*: Accessing Bank 1 requires specific SDK register manipulation (`GPIOBASE` offsets).

## 6. Critical Constraints

*   **Interrupt Latency**: Core 1's DMA ISR must complete well within the horizontal blanking period (~1.9µs) to maintain HDMI sync.
*   **CPU Cycle Sensitivity**: In the scanline generation phase, every nanosecond counts. Unoptimized logic (such as `if` statements inside pixel loops) will cause the HSTX FIFO to underrun, resulting in an immediate loss of video signal.
*   **Grounding**: The digital video tap is extremely sensitive to ground loops. A low-impedance shared ground is mandatory.
