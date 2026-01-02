# Neo Geo MVS MV1C Digital Video Specification

Technical specification for the digital video signals on the Neo Geo MVS MV1C arcade board.

## Signal Overview

The MVS MV1C generates 15-bit RGB video digitally, clocked at 6 MHz with composite sync (CSYNC). Signals are digital and can be tapped before the R2R DAC for direct digital capture.

### Video Signals

| Signal | Width  | Description                |
| ------ | ------ | -------------------------- |
| R[4:0] | 5 bits | Red channel (R4 is MSB)    |
| G[4:0] | 5 bits | Green channel (G4 is MSB)  |
| B[4:0] | 5 bits | Blue channel (B4 is MSB)   |
| PCLK   | 1 bit  | Pixel clock, 6 MHz         |
| CSYNC  | 1 bit  | Composite sync, active low |

**Color depth**: 15-bit (32,768 colors)
**Data valid**: RGB sampled on PCLK rising edge

## Timing Parameters

### Frame Timing

| Parameter         | Value                           |
| ----------------- | ------------------------------- |
| Frame rate        | 59.19 Hz (MVS) / 59.60 Hz (AES) |
| Active resolution | 320 x 224                       |

## Capture Strategy (NeoPico-HD)

To achieve "cinema quality" results without CPU overhead or drift, the following architecture is used:

### 1. PIO Hardware Synchronization

The PIO state machine handles the sub-microsecond timing of the 6MHz PCLK.

- **Start-of-Frame**: The C code detects VSYNC, resets the PIO, and triggers it via an IRQ precisely at the first active line.
- **Line Sync**: The PIO self-synchronizes to the CSYNC falling edge for every single line to prevent horizontal drift.

### 2. Zero-Overhead DMA

Pixel data is moved from the PIO FIFO to RAM entirely by the DMA controller.

- **Ping-Pong Buffering**: While DMA captures Line N into `buffer[0]`, the CPU processes Line N-1 from `buffer[1]`.
- **Parallelism**: This allows the CPU to perform RGB555->RGB565 conversion and OSD rendering without stalling the hardware capture.

### 3. Tap Points (MV1C Board)

| Signal | Location                 |
| ------ | ------------------------ |
| PCLK   | PC23 pin 11 (or GPIO 25) |
| Pixels | GPIO 26-42 (Contiguous)  |
| CSYNC  | R51 (or GPIO 43)         |

## Implementation Notes

- **Grounding**: A common ground between MVS and Pico is required to prevent "ghosting" or noise in the digital signal.
- **Clock Unification**: Both video and audio systems are unified at a 252 MHz system clock to support HDMI 480p audio data islands.
