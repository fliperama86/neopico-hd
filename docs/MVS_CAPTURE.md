# Neo Geo MVS Video Capture with Raspberry Pi Pico

## Project Overview

This project captures video frames from a Neo Geo MVS arcade board using a Raspberry Pi Pico and PIO (Programmable I/O). The goal is to understand MVS video signals and eventually capture full RGB frames.

## Hardware Setup

### Connections

| Pico GPIO | MVS Signal | Source Location | Description |
|-----------|------------|-----------------|-------------|
| GP0 | CSYNC | R51 | Composite sync (HSYNC + VSYNC) |
| GP1 | C2 (PCLK) | PC23 pin 11 | 6 MHz pixel clock |
| GP2 | R4 | R61 | Red MSB (bit 4 of 5-bit red) |
| GP3-GP6 | R0-R3 | R53,55,57,59 | Red bits 0-3 (future) |
| GP7-GP11 | G0-G4 | R69,70,72,74,75 | Green channel (future) |
| GP12-GP16 | B0-B4 | R54,56,58,60,62 | Blue channel (future) |

### MVS Video Signal Specifications

Based on cps2_digiav neogeo_frontend.v and our measurements:

```
Pixel Clock:    6.001 MHz (C2 signal)
Horizontal:
  Total pixels: 384 per line
  Active:       320 pixels
  Sync length:  29 pixels
  Back porch:   28 pixels
  Front porch:  7 pixels (calculated)

Vertical:
  Total lines:  264 per frame
  Active:       224 lines
  Sync length:  3 lines
  Back porch:   21 lines
  Front porch:  16 lines (calculated)

Frame Rate:     59.19 Hz (MVS) / 59.60 Hz (AES)
HSYNC Freq:     15.625 kHz (6MHz / 384)
Pixel time:     ~167 ns (1 / 6MHz)
Line time:      ~64 µs (384 / 6MHz)
Frame time:     ~16.9 ms (264 lines * 64µs)
```

## Progressive Implementation (Baby Steps Approach)

### Step 1: Pixel Clock Validation ✅

**Goal**: Verify we can capture the 6 MHz pixel clock

**Implementation**: Used existing frequency counter PIO program

**Results**:
```
Measured: 6.001 MHz ✓
Expected: 6.000 MHz
Error: 0.017% (excellent!)
```

### Step 2: CSYNC Decoding ✅

**Goal**: Understand composite sync signal structure

**Challenge**: CSYNC contains both HSYNC and VSYNC with equalization pulses

**Measured**: ~506 kHz CSYNC edges (both rising and falling)

**Analysis**:
- Expected HSYNC only: ~15.6 kHz (264 lines/frame × 59.19 fps)
- Measured: 506 kHz total edges
- CSYNC edges per frame: 8,552 edges
- Normal scanline edges: 528 (264 lines × 2 edges each)
- VSYNC equalization edges: ~8,024 extra edges during vertical sync

**Key Insight**: CSYNC has rapid equalization and serration pulses during VSYNC period. We're counting all edges, not just HSYNC.

### Step 3: Frame Structure Validation ✅

**Goal**: Derive frame parameters from measurements

**Calculations**:
```
Pixels per frame = 384 × 264 = 101,376 pixels
Frame rate = 6,001,300 Hz / 101,376 = 59.20 fps ✓
HSYNC rate = 6,001,300 Hz / 384 = 15,628 Hz ✓
```

All calculated values match MVS specifications!

### Step 4: Pixel Data Capture ✅

**Goal**: Sample actual video data (R4 channel only)

**Implementation**:
- Created `mvs_pixel_capture` program (in mvs_sync.pio) to sample R4 on each pixel clock
- PIO uses auto-push every 32 pixels
- C code tracks CSYNC to identify scanlines

**Initial Test Results** (Metal Slug X gameplay):
```
PCLK: 6.01 MHz
CSYNC: 16.17 kHz (filtered HSYNC)
R4: 302.75 kHz edge rate
Frame rate: 59.25 fps
R4 edges/frame: 5,110 (5.0% of pixels)
Activity: LOW-MEDIUM (typical game graphics)
```

**Observation**: R4 edge count changes with screen content:
- Static screens: Lower activity
- Explosions/bright areas: Higher activity
- This validates real-time capture!

### Step 5: Full Frame Capture (In Progress) 🔄

**Goal**: Capture complete 320×224 frame and save as viewable image

**Implementation**:
- Allocate 8,960-byte frame buffer (320×224 ÷ 8)
- Capture all 224 active lines
- Output as PBM (Portable Bitmap) format over USB
- User saves to file and views

**Expected with MVS color test pattern**:
- 4-square pattern: White, Red, Green, Blue
- R4 channel shows: White (1), Red (1), Green (0), Blue (0)
- Should see 2 bright quadrants, 2 dark quadrants

## PIO Programming Techniques Used

### Frequency Counter PIO

```pio
.wrap_target
    wait 0 pin 0        ; Wait for falling edge
    wait 1 pin 0        ; Wait for rising edge
    jmp x-- loop        ; Count in X register
.wrap
```

**Features**:
- Counts edges in hardware using X register
- X counts down from 0xFFFFFFFF
- C code reads periodically via `pio_sm_exec()`
- Zero CPU overhead during counting

### Pixel Capture PIO

```pio
.wrap_target
    wait 1 gpio 1       ; Wait for pixel clock
    in pins, 1          ; Sample R4 bit
.wrap
```

**Configuration**:
- Auto-push every 32 bits
- Shift right (LSB first)
- FIFO join for 8-word depth
- Runs at full speed (125 MHz / no divider)

## C Code Architecture

### CSYNC Tracking (GPIO)

```c
bool csync = gpio_get(PIN_CSYNC);
if (last_csync && !csync) {
    // HSYNC falling edge detected
    current_line++;
}
```

Simple GPIO polling to detect line boundaries.

### Frame Synchronization

```c
// Wait for VSYNC (CSYNC low for extended period)
int low_count = 0;
while (low_count < 100) {
    if (gpio_get(PIN_CSYNC)) {
        low_count = 0;
    } else {
        low_count++;
    }
}
```

Detects VSYNC by looking for sustained low CSYNC.

### Pixel Data Reading

```c
for (int word = 0; word < 10; word++) {
    while (pio_sm_is_rx_fifo_empty(pio, sm)) {
        tight_loop_contents();
    }
    uint32_t pixel_word = pio_sm_get(pio, sm);
    // Store 32 pixels...
}
```

Reads 10 words (320 pixels) per scanline from PIO FIFO.

## Key Learnings

### 1. Composite Sync Complexity

**Discovery**: CSYNC is not just HSYNC + VSYNC pulses. During vertical sync:
- Equalization pulses before VSYNC (half-line rate)
- Serration pulses during VSYNC (maintain horizontal sync)
- More equalization pulses after VSYNC

**Implication**: Must use timing or pixel counting to distinguish HSYNC from VSYNC pulses.

### 2. PIO is Perfect for Video Capture

**Why PIO works so well**:
- 6 MHz signal << 125 MHz PIO clock (20x headroom)
- Hardware sampling synchronized to external clock
- FIFO buffering prevents data loss
- Zero CPU intervention during capture
- Can run multiple state machines in parallel

### 3. Baby Steps Approach Works

**Methodology that succeeded**:
1. Start with clock frequency validation
2. Understand sync structure through measurement
3. Derive and validate timing parameters
4. Capture single bit before full RGB
5. Verify with known test patterns

Each step built confidence before moving forward.

### 4. USB Bandwidth is Not a Bottleneck

**Frame size**: 320×224×1bit = 8,960 bytes
**Frame rate**: 59.19 fps
**Data rate**: 8,960 × 59.19 = ~530 KB/s
**USB capacity**: 12 Mbps = 1.5 MB/s

We're using ~35% of USB 1.1 bandwidth for 1-bit capture.
Full RGB (15-bit) would use ~8 MB/s, requiring USB compression or lower frame rate.

## Comparison with cps2_digiav FPGA Implementation

| Feature | cps2_digiav (FPGA) | Our Pico Implementation |
|---------|-------------------|------------------------|
| **Hardware** | Cyclone FPGA | RP2040 (Pico) |
| **Clock Sync** | Hardware PLLs | PIO `wait` on external clock |
| **CSYNC Decode** | Verilog state machine | C GPIO polling + timing |
| **Pixel Capture** | Parallel 15-bit wide bus | PIO serial sampling |
| **Processing** | Real-time HDMI upscaling | Store/analyze frames |
| **Latency** | <1 line (~40µs) | 1 frame (~16.9ms) |
| **Output** | HDMI 1080p@59Hz | USB / frame dump |
| **Cost** | ~$60 | ~$4 |
| **Complexity** | High (Quartus, Verilog) | Medium (C + PIO assembly) |

**Conclusion**: FPGA is necessary for real-time HDMI output, but Pico is perfect for frame capture and analysis.

## Performance Analysis

### Timing Budget per Scanline

```
Line duration:      64 µs
Active period:      53.3 µs (320 pixels @ 6MHz)
Horizontal blank:   10.7 µs

PIO capture time:   53.3 µs (hardware, no CPU)
C processing time:  ~1-2 µs (read FIFO, store buffer)
Margin:             ~9 µs ✓
```

### Memory Usage

```
Frame buffer:       8,960 bytes (1-bit)
PIO program:        ~100 bytes
Stack/variables:    ~1 KB
Total:              ~10 KB of 264 KB RAM (3.8%)
```

Plenty of room for:
- Full 15-bit RGB capture: ~134 KB (51% of RAM)
- Double buffering: Would fit!
- Multiple frames: Could store 2-3 frames

## Next Steps

### Immediate (Step 5 completion)

1. **Test full frame capture** with MVS color test pattern
2. **Validate PBM output** - check if image is recognizable
3. **Adjust timing** if needed (horizontal blanking delay)

### Short Term

1. **Add all RGB channels** (15 GPIOs total)
2. **Capture full color frames** (5+5+5 bits)
3. **Implement better VSYNC detection** (possibly in PIO)

### Medium Term

1. **Add DARK/SHADOW signals** for brightness control
2. **Implement DMA** for zero-CPU frame capture
3. **Add frame differencing** to detect motion/changes
4. **Compression** for faster USB transfer

### Long Term

1. **Multi-frame capture** for animations
2. **Frame rate analysis** (detect dropped frames, timing issues)
3. **Color palette extraction** for sprites/backgrounds
4. **Statistics engine** for game analysis

## Code Structure

```
neopico-hd/
├── src/
│   ├── main.c               # Main capture implementation (DMA-based R4 capture)
│   └── mvs_sync.pio         # CSYNC decoder + pixel capture PIO programs
├── docs/
│   ├── MVS_CAPTURE.md       # This file
│   ├── MVS_DIGITAL_VIDEO.md # Complete signal specifications
│   ├── PROJECT_STATUS.md    # Current progress tracking
│   ├── CSYNC_IMPLEMENTATION_PLAN.md # Development methodology
│   └── TIMING_OFFSET_ANALYSIS.md    # Frame offset analysis
├── CMakeLists.txt           # Build configuration
├── README.md                # Project overview
└── scripts/
    └── build.sh             # Build script
```

## References

### MVS Technical Resources

1. **cps2_digiav project**: https://github.com/marqs85/cps2_digiav
   - neogeo_frontend.v: MVS timing parameters
   - Install docs: Signal hookup points for MV1C

2. **Neo Geo Development Wiki**: https://wiki.neogeodev.org/
   - Video timing specifications
   - Hardware architecture

3. **PicoDVI-N64 project**: Inspiration for PIO video capture techniques

### Pico SDK Resources

1. **PIO Documentation**: https://datasheets.raspberrypi.com/pico/raspberry-pi-pico-c-sdk.pdf
2. **Hardware DMA**: For future zero-CPU capture implementation
3. **USB Serial**: stdio over USB for frame data transfer

## Acknowledgments

- **marqs85** for cps2_digiav FPGA design and MVS timing documentation
- **Raspberry Pi Foundation** for excellent PIO documentation
- **Konrad Beckmann** for PicoDVI-N64 project showing PIO video capture techniques
