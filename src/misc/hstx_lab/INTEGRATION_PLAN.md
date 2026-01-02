# HSTX Lab + MVS Video Capture Integration Plan

## Status: COMPLETE ✓

All phases successfully implemented. Live MVS video capture working via HSTX output.

## Overview

Integrate MVS video capture from the main neopico-hd firmware into the HSTX lab,
enabling real Neo Geo video display via the RP2350's native HSTX peripheral.

## Resource Mapping

| Resource | Video Capture | HSTX Output | Conflict? |
|----------|---------------|-------------|-----------|
| **Core** | Core 0 | Core 1 | No (separated) |
| **PIO** | PIO1 (SM0, SM1) | HSTX peripheral | No |
| **GPIOs** | 25-43 (Bank 1) | 12-19 (Bank 0) | No |
| **DMA** | Channel 2+ | Channels 0, 1 | No (claimed first) |
| **Clock** | Works at any clock | Needs 126MHz | No |

## Architecture

```
+------------------------------------------------------------------+
|                         RP2350B                                   |
+----------------------------+-------------------------------------+
|         CORE 0             |            CORE 1                   |
|    (Video Capture)         |        (HSTX Output)                |
+----------------------------+-------------------------------------+
|  PIO1 SM0: Sync detect     |  HSTX Peripheral                    |
|  PIO1 SM1: Pixel capture   |  DMA CH0/CH1: Ping-pong             |
|  DMA CH2: Line transfer    |  DMA IRQ: Per-line handler          |
|                            |                                     |
|  wait_for_vsync()          |  pixel_double_line()                |
|  capture_line() x 224      |  build_data_islands()               |
|  convert_rgb555_to_rgb565()|  service_audio_ring()               |
|                            |                                     |
|  --------------------------+-----------------------------------  |
|           WRITES ----------+-> framebuf[320x240] <--- READS      |
|                            |                                     |
+----------------------------+-------------------------------------+
         |                              |
    GPIOs 25-43                    GPIOs 12-19
    (MVS signals)                  (HDMI output)
```

## Implementation Phases

### Phase 1: Make HSTX Multicore ✓

**Goal**: Move HSTX output to Core 1, freeing Core 0 for capture.

**Changes**:
1. Created `core1_entry()` function containing all HSTX initialization
2. Moved DMA IRQ handler registration to Core 1
3. Launch Core 1 via `multicore_launch_core1()`
4. Made `video_frame_count` volatile for cross-core visibility

**Result**: Rainbow gradient displays correctly with Core 1 handling output.

### Phase 2: Port Video Capture ✓

**Goal**: Add MVS capture capability to Core 0.

**Changes**:
1. Added `video_capture.c` to hstx_lab build in CMakeLists.txt
2. Added PIO header generation for `video_capture.pio`
3. Added `HSTX_LAB_BUILD` define to exclude PicoDVI dependencies
4. Modified `hardware_config.h` with `#ifndef HSTX_LAB_BUILD` guards
5. Claim DMA channels 0,1 for HSTX BEFORE Core 1 launch

**Result**: Capture initializes without crashing HSTX output.

### Phase 3: Single Frame Capture Test ✓

**Goal**: Capture one MVS frame and display it.

**Key Discovery**: Pixel sampling timing was incorrect. Added NOP delay after
PCLK rising edge in PIO program to allow data setup time.

**Changes to video_capture.pio**:
```
pixel_loop:
    wait 0 pin 0               ; Wait for PCLK LOW
    wait 1 pin 0               ; Wait for PCLK HIGH (rising edge)
    nop                        ; Data setup time (MVS needs ~1 cycle after edge)
    in pins, 18                ; Sample GP25-42
    jmp x-- pixel_loop
```

**Result**: Sharp, artifact-free single frame capture displayed via HSTX.

### Phase 4: Continuous Capture ✓

**Goal**: Live video feed from MVS.

**Implementation**: Simple continuous capture loop on Core 0.

**Key Finding**: Double-buffering NOT required! Single shared framebuffer works
without visible tearing. Both cores access framebuf concurrently with no issues.

**Result**: Smooth live video from MVS displayed on HDMI.

## Technical Details

### Format Compatibility

| Aspect | Video Capture | HSTX Output | Match? |
|--------|---------------|-------------|--------|
| Resolution | 320x224 (+padding = 320x240) | 640x480 (2x scaled) | Yes |
| Pixel Format | RGB565 (converted from RGB555) | RGB565 | Yes |
| Frame Rate | ~59.19 Hz | 60 Hz | Close enough |

### Pixel Scaling

- Horizontal: 320→640 via `p | (p << 16)` packing (each pixel duplicated)
- Vertical: 240→480 via `fb_line = active_line / 2` (each line shown twice)

### Memory Layout

```
framebuf[320x240]:
  Lines 0-7:    Gradient background (unused MVS padding)
  Lines 8-231:  Captured MVS content (224 lines)
  Lines 232-239: Gradient background (unused MVS padding)
```

### DMA Channel Allocation

- Channel 0: HSTX ping (claimed before Core 1 launch)
- Channel 1: HSTX pong (claimed before Core 1 launch)
- Channel 2+: Video capture (claimed dynamically by video_capture_init)

## Key Learnings

1. **Timing Matters**: MVS pixel data needs setup time after PCLK rising edge.
   Adding a single NOP (~8ns at 126MHz) fixed all capture artifacts.

2. **No Double Buffer Needed**: Despite theoretical race conditions, single
   framebuffer works perfectly. RP2350's memory bandwidth handles both cores.

3. **Core Separation Works Well**: PIO1 capture on Core 0 + HSTX on Core 1
   run completely independently without resource conflicts.

4. **HSTX is Simpler**: Compared to PIO-based PicoDVI, HSTX requires less
   code and runs at lower system clock (126MHz vs 252MHz).
