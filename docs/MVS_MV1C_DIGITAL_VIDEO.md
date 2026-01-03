# Neo Geo MVS MV1C Digital Video Specification

Technical specification for the digital video signals on the Neo Geo MVS MV1C arcade board and the RP2350-specific capture implementation.

## 1. Signal Overview

The MVS MV1C generates 15-bit RGB video digitally, clocked at 6 MHz with composite sync (CSYNC).

### Video Signals

| Signal | Width  | Description                | Tap Point (MV1C) | GPIO (Pico) |
| ------ | ------ | -------------------------- | ---------------- | ----------- |
| PCLK   | 1 bit  | Pixel clock, 6 MHz         | PC23 pin 11      | GPIO 25     |
| R[4:0] | 5 bits | Red MSB -> LSB             | R2R Input        | GPIO 26-30  |
| G[4:0] | 5 bits | Green MSB -> LSB           | R2R Input        | GPIO 31-35  |
| B[4:0] | 5 bits | Blue MSB -> LSB            | R2R Input        | GPIO 36-40  |
| DARK   | 1 bit  | Intensity control          | Pre-DAC          | GPIO 41     |
| SHADOW | 1 bit  | Intensity control          | Pre-DAC          | GPIO 42     |
| CSYNC  | 1 bit  | Composite sync, active low | R51              | GPIO 43     |

**Capture Logic**: PIO1 samples GPIO 25-42 (18 pins) as a contiguous block on the PCLK rising edge.

## 2. Capture Strategy

### PIO Hardware Synchronization

- **Line Sync**: PIO1 self-synchronizes to the CSYNC falling edge for every single line to prevent horizontal drift.
- **Start-of-Frame**: Core 0 detects VSYNC pulses, resets the PIO state, and triggers the capture IRQ precisely at the first active video line.

### Zero-Overhead DMA

- **Ping-Pong Buffering**: DMA moves raw pixel words to RAM in the background. While DMA captures Line N into `buffer[0]`, the CPU processes Line N-1 from `buffer[1]`.
- **Zero Jitter**: This offloads the high-speed work from the CPU, allowing Core 0 to handle conversion and OSD tasks without missing pixels.

### Hardware-Accelerated Pixel Conversion

The Neo Geo uses two special signals, **DARK** and **SHADOW**, to modify pixel brightness per-pixel. To handle this with minimum CPU overhead, NeoPico-HD uses the RP2350's **Hardware Interpolator** and a **Runtime-generated Lookup Table (LUT)**.

1.  **Intensity LUT**: A 256KB LUT (131,072 entries) is generated at boot. It maps the 17-bit raw capture (RGB555 + DARK + SHADOW) to the final RGB565 value.
2.  **Order of Operations**:
    *   SHADOW (50% dimming) is applied first.
    *   SHADOW forces DARK to 1.
    *   5-to-8 bit color expansion.
    *   DARK (-4 intensity) is applied last.
3.  **Interpolator Logic**: `interp0` is configured with a mask of bits 1-17. This skips the PCLK bit and automatically multiplies the index by 2 (byte offset for `uint16_t`), allowing a single-cycle address generation for the LUT.

## 3. RP2350 Hardware Platform Notes (Bank 1)

Capturing from GPIO 25-43 requires using the RP2350's **Bank 1** features and managing specific hardware/SDK quirks.

### GPIOBASE Register

- **PIO0/1 only**: PIO0 and PIO1 have a `GPIOBASE` register (offset `0x168`) that defines a 32-pin window.
- **Current Config**: `GPIOBASE` is set to **16**, giving the PIO access to **GPIO 16 through 47**.
- **PIO2 Limitation**: PIO2 lacks this register and is fixed to Bank 0 (GPIO 0-31).

### PIO Instruction Addressing

- **Relative Addressing**: In Bank 1 mode, instructions like `WAIT GPIO <n>` are relative to `GPIOBASE`.
- **Recommendation**: Use **`WAIT PIN <n>`** instead of `WAIT GPIO`. `WAIT PIN` is relative to the SM's `IN_BASE` and avoids the SDK's automatic (and often incorrect) XOR bit-flipping during program loading.

### RP2350-E9 Erratum (Sticky Pins)

- **The Bug**: Internal pull-downs can latch GPIOs at ~2.1V, preventing a logic "Low".
- **Mitigation**: `gpio_disable_pulls()` is called on all capture pins. Avoid `pio_gpio_init()` as it can re-enable default pulls.

## 4. Implementation Constraints

- **Clock**: The system runs at **126 MHz** to provide a perfect 25.2 MHz pixel clock (div 5) for HDMI.
- **Grounding**: A solid shared ground between MVS and Pico is mandatory to prevent digital noise.
