# Neo Geo MVS MV1C Digital Video Specification

Technical specification for the digital video signals on the Neo Geo MVS MV1C arcade board and the RP2350-specific capture implementation.

## 1. Signal Overview

The MVS MV1C generates 15-bit RGB video digitally, clocked at 6 MHz with composite sync (CSYNC).

### Video Signals

| Signal | Width  | Description                | Tap Point (MV1C) | GPIO (Pico) |
| ------ | ------ | -------------------------- | ---------------- | ----------- |
| CSYNC  | 1 bit  | Composite sync, active low | R51              | GPIO 27     |
| PCLK   | 1 bit  | Pixel clock, 6 MHz         | PC23 pin 11      | GPIO 28     |
| B[4:0] | 5 bits | Blue MSB -> LSB            | R2R Input        | GPIO 29-33  |
| G[4:0] | 5 bits | Green MSB -> LSB           | R2R Input        | GPIO 34-38  |
| R[4:0] | 5 bits | Red MSB -> LSB             | R2R Input        | GPIO 39-43  |
| SHADOW | 1 bit  | Intensity control          | Pre-DAC          | GPIO 44     |
| DARK   | 1 bit  | Intensity control          | Pre-DAC          | GPIO 45     |

**Capture Logic**: PIO1 samples GPIO 27-45 (19 pins) as a contiguous block. IN_BASE = GP27; CSYNC is at bit 0, PCLK at bit 1, then B/G/R, SHADOW at bit 17, and DARK at bit 18. Sync and pixel SMs use the same window; sync waits on CSYNC (GP27), pixel SM samples on PCLK (GP28).

## 2. Capture Strategy

### PIO Hardware Synchronization

-   **Line Sync**: PIO1 self-synchronizes to the CSYNC falling edge for every single line to prevent horizontal drift.
-   **Start-of-Frame**: Core 0 detects VSYNC pulses, resets the PIO state, and triggers the capture IRQ precisely at the first active video line.

### Zero-Overhead DMA

-   **Ping-Pong Buffering**: DMA moves raw pixel words to RAM in the background. While DMA captures Line N into `buffer[0]`, the CPU processes Line N-1 from `buffer[1]`.
-   **Capture Headroom**: This offloads pixel transfer from the CPU while Core 0 performs conversion and frame management. OSD work remains on Core 1.

### Hardware-Accelerated Pixel Conversion

The Neo Geo uses two special signals, **DARK** and **SHADOW**, to modify pixel brightness. To handle this with minimum CPU overhead, NeoPico-HD uses a runtime-generated LUT in Core 0 capture.

1.  **Stable Digital Build** (`NEOPICO_ENABLE_DARK_SHADOW=OFF`):
    -   A 32K-entry LUT (64 KiB) converts corrected RGB555 -> standard RGB565.
    -   All 32,768 source colors retain their RGB555 codes. Green expands from five to six bits by repeating its MSB.
    -   SHADOW/DARK bits are captured but ignored in pixel conversion.
2.  **Optional Normal-Color Menu** (`NEOPICO_MVS_COLOR_MODEL_MENU=ON`):
    -   The persistent OSD values are `Digital` and `Analog`; `Digital` is the stable default.
    -   `Digital` generates exactly the same LUT as the stable build.
    -   `Analog` generates normal-pixel levels from the pinned Neo Geo DAC resistor-network model. It can change ordinary colors even when no effect signal is active, and remains experimental rather than a measurement of the target MV1C.
    -   Both choices ignore SHADOW/DARK. The OSD shows only the selected model's one-line description.
    -   Both 64 KiB LUTs are generated at boot. Moving the selection previews through one active base-pointer change at the next input VSYNC. All lines in a frame use the same pointer.
    -   SELECT restores the committed model. START/MENU commits and persists the preview without rebooting.
    -   The active-pixel loop remains one halfword LUT read with no model branch. The second table adds 64 KiB compared with the stable build.
    -   Persistence is queued to Core 0 and written after a complete input frame, or after the no-signal timeout. Capture is then reset for clean synchronization while Core 1 continues HDMI output from SRAM. No firmware reboot occurs, although the displayed frame can be held briefly during the flash write.
    -   The live save and capture-resynchronization path remains experimental until hardware validation.
3.  **Separate Effect Experiment** (`NEOPICO_ENABLE_DARK_SHADOW=ON`):
    -   An 8,448-byte split RGB565 LUT represents all four independent states: normal, SHADOW, DARK, and DARK+SHADOW.
    -   Each pixel performs two branchless halfword table reads, one for R/G and one for B, then combines them with OR.
    -   `NEOPICO_MVS_EFFECT_MODEL=MISTER` is the default and follows the pinned MiSTer digital implementation exactly before RGB565 packing.
    -   `NEOPICO_MVS_EFFECT_MODEL=MAME` follows MAME's pinned Neo Geo resistor-network model. This is an analog emulation model, not a measurement of the target MV1C.
    -   The selected model is exact in the generated table. RGB565 transport still truncates the modeled 8-bit components to 5/6/5 bits.
    -   This path produced bottom-screen pixel jitter in hardware testing. It remains default off and cannot be combined with the normal-color menu.
4.  **Capture Width**:
    -   Capture uses 19 bits (RGB555 + SHADOW + DARK).

The model references are pinned to [MiSTer Neo Geo commit `2325e6c`](https://github.com/MiSTer-devel/NeoGeo_MiSTer/blob/2325e6c4303dc9a3fd554b18d9833e992ccd444f/neogeo.sv#L2205-L2222) and [MAME commit `e47c0f3`](https://github.com/mamedev/mame/blob/e47c0f33c5be3ee286ff65bed13458c2920340d2/src/mame/neogeo/neogeo_v.cpp#L23-L64).

## 3. RP2350 Hardware Platform Notes (Bank 1)

Capturing from GPIO 27-45 requires using the RP2350's **Bank 1** features and managing specific hardware/SDK quirks.

### GPIOBASE Register

-   **PIO0/1 only**: PIO0 and PIO1 have a `GPIOBASE` register (offset `0x168`) that defines a 32-pin window.
-   **Current Config**: `GPIOBASE` is set to **16**, giving the PIO access to **GPIO 16 through 47**.
-   **PIO2 Limitation**: PIO2 lacks this register and is fixed to Bank 0 (GPIO 0-31).

### PIO Instruction Addressing

-   **Relative Addressing**: In Bank 1 mode, instructions like `WAIT GPIO <n>` are relative to `GPIOBASE`.
-   **Recommendation**: Use **`WAIT PIN <n>`** instead of `WAIT GPIO`. `WAIT PIN` is relative to the SM's `IN_BASE` and avoids the SDK's automatic (and often incorrect) XOR bit-flipping during program loading.

### RP2350-E9 Erratum (Sticky Pins)

-   **The Bug**: Internal pull-downs can latch GPIOs at ~2.1V, preventing a logic "Low".
-   **Mitigation**: `gpio_disable_pulls()` is called on all capture pins. Avoid `pio_gpio_init()` as it can re-enable default pulls.

## 4. Implementation Constraints

-   **Clock**: The system runs at **126 MHz** to provide a perfect 25.2 MHz pixel clock (div 5) for HDMI.
-   **Grounding**: A solid shared ground between MVS and Pico is mandatory to prevent digital noise.
