# NeoPico HD - Neo Geo MVS Video Capture with Raspberry Pi Pico

A deterministic, zero-jitter video frame capture system for Neo Geo MVS arcade hardware using Raspberry Pi Pico (RP2040) with PIO and DMA.

## Project Overview

This project captures digital video frames from Neo Geo MVS arcade boards with hardware-synchronized precision. It uses the RP2040's PIO subsystem for deterministic signal sampling and DMA for zero-CPU-overhead data transfer.

### Current Status

- ✓ **Deterministic capture**: Hardware IRQ synchronization eliminates timing jitter
- ✓ **320×224 resolution**: Full active frame captured at 59.19 fps
- ✓ **Zero CPU overhead**: DMA transfers ~109K pixels per frame automatically
- ✓ **Perfect consistency**: Every frame captured at identical position (±0 pixels)
- ✓ **Working implementation**: Single-bit (R4) capture validated
- 🔨 **In progress**: 3-bit RGB (R4, G4, B4) color capture

## Key Files

### Source Code

- **src/main_dma.c** - **[CURRENT BUILD]** DMA-based frame capture with hardware IRQ synchronization (single-bit R4)
- **src/main_rgb.c** - Experimental RGB color capture integration (in development)
- **src/main.c** - Original implementation with polling-based synchronization (reference)
- **src/mvs_sync.pio** - PIO programs for CSYNC decoding and pixel sampling
- **src/CMakeLists.txt** - Build configuration (currently uses main_dma.c)

### Documentation

- **MVS_DIGITAL_VIDEO.md** - Complete MVS video signal specifications and capture implementation
- **CSYNC_IMPLEMENTATION_PLAN.md** - Development roadmap and methodology
- **PROJECT_STATUS.md** - Current project status and next steps
- **MVS_CAPTURE.md** - Capture experiments and results

### Build Tools

- **scripts/build.sh** - Build script for Pico SDK

## Hardware Setup

- **GP0 (Pin 1)**: CSYNC input - Composite sync from MVS
- **GP1 (Pin 2)**: PCLK input - 6 MHz pixel clock from MVS
- **GP2 (Pin 4)**: R4 input - Red MSB for 1-bit capture
- **GP3 (Pin 5)**: G4 input - Green MSB (for future 3-bit RGB)
- **GP4 (Pin 6)**: B4 input - Blue MSB (for future 3-bit RGB)

### MVS Hookup Points (MV1C Board)

- **CSYNC**: R51 (bottom side)
- **PCLK**: PC23 pin 11
- **R4**: R61 (LS273 side)
- **G4**: R75 (LS273 side)
- **B4**: R62 (LS273 side)
- **GND**: C6

See `MVS_DIGITAL_VIDEO.md` for complete hookup details.

## Building

```bash
./scripts/build.sh
```

The build script will:

1. Check for PICO_SDK_PATH environment variable
2. Create/update build directory
3. Run cmake and make with parallel compilation
4. Generate `build/src/neopico_hd.uf2` file

## Deploying

1. Connect your Pico in BOOTSEL mode (hold BOOTSEL button while plugging in USB)
2. Copy the UF2 file to the Pico:

```bash
cp build/src/neopico_hd.uf2 /Volumes/RPI-RP2/
```

The Pico will automatically reboot and start capturing.

## Capturing Frames

To capture a frame to PBM file:

```bash
# Connect and redirect output to file
cat /dev/tty.usbmodem* > frame.pbm

# Wait ~1 second for capture to complete, then Ctrl-C
# View the image
open frame.pbm
```

The output is a standard PBM (Portable Bitmap) file showing the captured 320×224 frame.

## How It Works

### Architecture Overview

The capture system uses three key components working in parallel:

1. **PIO State Machine 0 (Sync Decoder)**:

   - Counts pixel clocks between CSYNC edges
   - Pushes horizontal counter (h_ctr) values to FIFO
   - Enables C code to detect VSYNC by counting equalization pulses (h_ctr < 288)

2. **PIO State Machine 1 (Pixel Capture)**:

   - Waits at `wait 1 irq 4` instruction (blocked at deterministic point)
   - When triggered, samples R4 on every PCLK rising edge
   - Autopushes packed 32-bit words to FIFO

3. **DMA Controller**:
   - Transfers pixel data from PIO FIFO to RAM buffer
   - Paced by PIO (pio_get_dreq) - zero CPU involvement
   - Captures ~109K pixels per frame

### Hardware IRQ Synchronization (Zero Jitter)

**The Problem**: C code has variable timing (USB interrupts, processing delays), causing random frame alignment.

**The Solution**:

1. C code detects second VSYNC and waits for first normal HSYNC after VSYNC
2. Triggers pixel PIO via `pio_sm_exec(pio, sm_sync, pio_encode_irq_set(false, 4))`
3. Pixel PIO exits `wait 1 irq 4` immediately (hardware signal, no C code latency)
4. Result: Every capture starts at identical position (±0 pixels)

### Frame Processing

After DMA capture completes:

1. Post-process raw buffer to extract active 320×224 region
2. Skip blanking periods (horizontal: 38 pixels, vertical: 20 lines)
3. Output as PBM format for viewing

See `MVS_DIGITAL_VIDEO.md` for complete technical details.

---

## Optimization Techniques from PicoDVI-N64 Analysis

The following optimization techniques were identified from analyzing the [PicoDVI-N64 project](PicoDVI-N64/), which captures N64 video/audio at high speed and outputs via HDMI.

### 1. Compiler Optimization Flags

```c
#pragma GCC optimize("O3")
```

Forces maximum compiler optimization level for performance-critical code.

**Reference**: `PicoDVI-N64/software/apps/n64/main.c:9`

### 2. PIO Optimizations

#### Multiple State Machines in Parallel

PicoDVI-N64 uses **3 state machines simultaneously**:

- State machine 0: Video capture
- State machine 1: Audio capture
- State machine 2: Joybus (controller input)

Each runs independently at full speed, allowing parallel data capture from multiple sources.

**Reference**: `PicoDVI-N64/software/apps/n64/main.c:64-65`

#### FIFO Join - Doubling Buffer Depth

```c
sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
```

Joins TX and RX FIFOs to double the receive buffer from 4 words to 8 words. Critical for preventing data loss during burst transfers.

**Reference**: `PicoDVI-N64/software/apps/n64/n64.pio:186,212`

#### Auto-push/Auto-pull

```c
sm_config_set_in_shift(&c, true, true, 32);  // Auto-push after 32 bits
```

PIO automatically pushes data to FIFO when threshold is reached, eliminating the need for manual `push` instructions and saving cycles.

**Reference**: `PicoDVI-N64/software/apps/n64/n64.pio:189,215`

#### Precise Timing with Delay Modifiers

```pio
set x, 15 [13]      ; Set X and delay 13 cycles
in pins, 1          ; Sample at precise moment
jmp x-- loop [12]   ; Jump with 12 cycle delay
```

Uses `[N]` delay syntax to precisely time instructions for protocol requirements.

**Reference**: `PicoDVI-N64/software/apps/n64/joybus.pio:39-45`

#### Side-set for Zero-Overhead I/O

```pio
.side_set 2
out pc, 1    side 0b10    ; Output data + drive pins simultaneously
out pc, 1    side 0b01
```

Drives output pins while executing other instructions with zero timing overhead.

**Reference**: `PicoDVI-N64/software/libdvi/dvi_serialiser.pio:2,7-8`

### 3. DMA Optimizations

#### Chained DMA Channels

**Data + Control channel pairs** that automatically restart:

- Data channel: Transfers actual data
- Control channel: Resets data channel's address/count
- Chains form infinite loop with zero CPU intervention

```c
channel_config_set_chain_to(&c_audio_pio_data, dma_ch_audio_pio_ctrl);
```

**Reference**: `PicoDVI-N64/software/apps/n64/main.c:281-304`

#### DMA Triggered by PIO DREQ

```c
channel_config_set_dreq(&c_audio_pio_data, pio_get_dreq(pio, sm_audio, false));
```

DMA automatically pulls from PIO RX FIFO when data is available. PIO generates DREQ signal, hardware handles the rest.

**Reference**: `PicoDVI-N64/software/apps/n64/main.c:283`

#### DMA Timer for Precise Sampling

```c
dma_timer_claim(0);
set_audio_sampling_parameters(g_config.audio_out_sample_rate);
channel_config_set_dreq(&c_audio_buffer_data, DREQ_DMA_TIMER0);
```

Uses hardware timer to trigger DMA transfers at exact sample rates (e.g., 48 kHz audio). Ensures consistent timing without CPU involvement.

**Reference**: `PicoDVI-N64/software/apps/n64/main.c:323-327`

#### Ring Buffer + DMA Write Pointer Tracking

```c
inst->audio_ring.write = (((uint32_t) dma_hw->ch[chan].write_addr)
                          - ((uint32_t) buffer_base)) / 4;
```

Reads DMA hardware register directly to determine current write position. Enables lock-free producer/consumer pattern.

**Reference**: `PicoDVI-N64/software/libdvi/dvi.c:437-440`

#### IRQ-Quiet Channels

```c
channel_config_set_irq_quiet(&c_audio_pio_data, true);
```

Disables IRQ generation for background DMA channels, reducing interrupt overhead for autonomous transfers.

**Reference**: `PicoDVI-N64/software/apps/n64/main.c:285,298`

### 4. Multi-Core Architecture

PicoDVI-N64 uses both cores for parallel processing:

- **Core 0**: Captures N64 video/audio from PIO, processes pixel data
- **Core 1**: Encodes TMDS and outputs DVI/HDMI signal

Communication via **lock-free queues**:

```c
queue_init_with_spinlock(&inst->q_tmds_valid, sizeof(void*), 8, spinlock_num);
queue_init_with_spinlock(&inst->q_colour_valid, sizeof(void*), 8, spinlock_num);
```

**Reference**: `PicoDVI-N64/software/apps/n64/main.c:75-81,208` and `PicoDVI-N64/software/libdvi/dvi.c:38-41`

### 5. Memory Placement Optimizations

#### Scratch RAM for Critical Code

```c
#define __dvi_func_x(f) __scratch_x(__STRING(f)) f
```

Places time-critical functions in the 4KB scratch RAM (single-cycle access) rather than flash (multi-cycle with cache misses).

**Reference**: `PicoDVI-N64/software/libdvi/dvi.c:13`

#### Not-in-Flash Functions

```c
#define __dvi_func(f) __not_in_flash_func(f)
```

Moves frequently-called functions from flash to main RAM for faster execution.

**Reference**: `PicoDVI-N64/software/libdvi/dvi.c:12`

### 6. System Clock Overclocking

```c
vreg_set_voltage(VREG_VSEL);
sleep_ms(10);
set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);  // 252 MHz!
```

PicoDVI-N64 runs at **252 MHz** (2x the standard 125 MHz) to meet HDMI timing requirements. Steps:

1. Increase core voltage first
2. Wait for voltage to stabilize
3. Increase clock frequency

**Reference**: `PicoDVI-N64/software/apps/n64/main.c:168-172`

### 7. Advanced PIO Techniques

#### Custom Wrap Points

```c
sm_config_set_wrap(&c, offset + joybus_offset_joybus_rx_good_data,
                    offset + joybus_offset_joybus_rx_wrap);
```

Sets custom program loop boundaries instead of using `.wrap_target` and `.wrap` directives, allowing more complex control flow.

**Reference**: `PicoDVI-N64/software/apps/n64/joybus.pio:87`

#### Register Pre-initialization via pio_sm_exec()

```c
pio_sm_exec(pio, sm, pio_encode_set(pio_y, 1));  // Set Y=1 before starting
```

Executes PIO instructions from C code to initialize registers before enabling state machine. Used to set up constants or initial state.

**Reference**: `PicoDVI-N64/software/apps/n64/joybus.pio:102-103`

---

## Potential Optimizations for This Project

Based on the PicoDVI-N64 analysis, here are optimizations that could be applied:

### 1. DMA-Based Counter Reading

Replace periodic `pio_sm_exec()` calls with chained DMA that automatically snapshots the counter:

- DMA Timer triggers read every second
- Data channel reads PIO register
- Control channel restarts data channel
- Zero CPU overhead

### 2. FIFO Join

Enable FIFO join if streaming counter values:

```c
sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
```

### 3. Move to Scratch RAM

For sub-microsecond precision timing, move IRQ handler to scratch RAM:

```c
__scratch_x("counter_irq") void counter_irq_handler(void) { ... }
```

### 4. Overclock for Higher Frequencies

If measuring signals above 10 MHz:

```c
vreg_set_voltage(VREG_VOLTAGE_1_20);
set_sys_clock_khz(250000, true);
```

### 5. Multi-Channel Measurements

Use additional state machines to measure multiple signals simultaneously, similar to how PicoDVI-N64 captures video + audio + controller data in parallel.

### 6. IRQ-Driven Updates

Instead of polling every second, use a timer interrupt for more precise measurement intervals:

```c
add_repeating_timer_ms(1000, timer_callback, NULL, &timer);
```

---

## License

This project is for educational purposes, demonstrating efficient PIO usage on the RP2040.

PicoDVI-N64 analysis based on code by:

- Copyright (c) 2022-2023 Konrad Beckmann
- Copyright (c) 2023 Polprzewodnikowy / Mateusz Faderewski
- SPDX-License-Identifier: BSD-3-Clause
