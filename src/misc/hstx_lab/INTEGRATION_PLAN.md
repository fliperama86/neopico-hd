# HSTX Lab + MVS Video Capture Integration Plan

## Overview

Integrate MVS video capture from the main neopico-hd firmware into the HSTX lab,
enabling real Neo Geo video display via the RP2350's native HSTX peripheral.

## Resource Mapping

| Resource | Video Capture | HSTX Output | Conflict? |
|----------|---------------|-------------|-----------|
| **Core** | Core 0 (blocking) | Core 0 (IRQ-driven) | **YES** |
| **PIO** | PIO1 (SM0, SM1) | HSTX peripheral | No |
| **GPIOs** | 25-43 (Bank 1) | 12-19 (Bank 0) | No |
| **DMA** | 1 channel (dynamic) | Channels 0, 1 | Need care |
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

### Phase 1: Make HSTX Multicore

**Goal**: Move HSTX output to Core 1, freeing Core 0 for capture.

**Changes**:
1. Create `core1_entry()` function containing all HSTX initialization
2. Move DMA IRQ handler registration to Core 1
3. Launch Core 1 via `multicore_launch_core1()`
4. Core 0 main loop handles framebuffer updates (for now, test pattern)

**Validation**: Rainbow gradient + bouncing box still works after change.

### Phase 2: Port Video Capture

**Goal**: Add MVS capture capability to Core 0.

**Files to add**:
- `video_capture.c` (ported from main firmware)
- `video_capture.h` (ported from main firmware)
- `video_capture.pio` (copied from main firmware)

**Changes**:
1. Add PIO header generation to CMakeLists.txt
2. Initialize PIO1 on Core 0
3. Claim DMA channel 2 (avoid 0/1 used by HSTX)
4. Apply GPIOBASE=16 hack for Bank 1 GPIO access

**Validation**: Capture initializes without crashing HSTX output.

### Phase 3: Single Frame Capture Test

**Goal**: Capture one MVS frame and display it.

**Changes**:
1. Core 0: Call `video_capture_init()` after Core 1 launches
2. Core 0: Call `video_capture_frame()` once
3. Captured frame appears in shared `framebuf[]`
4. Core 1 displays it via HSTX (already running)
5. Stop capture, leave display running with static image

**Validation**: Real MVS video frame visible on HDMI output.

### Phase 4: Continuous Capture (Future)

**Goal**: Live video feed from MVS.

**Considerations**:
- Frame buffer race condition (Core 0 writes, Core 1 reads)
- MVS 59.19Hz vs HDMI 60Hz timing mismatch
- Double-buffering or vsync coordination needed
- Frame drop/repeat strategy for rate conversion

## Technical Details

### Format Compatibility

| Aspect | Video Capture | HSTX Output | Match? |
|--------|---------------|-------------|--------|
| Resolution | 320x224 (+padding = 320x240) | 320x240 | Yes |
| Pixel Format | RGB565 (converted from RGB555) | RGB565 | Yes |
| Frame Rate | ~59.19 Hz | 60 Hz | Close |

### Memory Bandwidth

- HSTX DMA: ~15 MB/s (640x480x2x60)
- Capture DMA: ~7 MB/s (384x224x4x60)
- Total: ~22 MB/s vs RP2350 bus (~200+ MB/s) - acceptable

### DMA Channel Allocation

- Channel 0: HSTX ping
- Channel 1: HSTX pong
- Channel 2+: Video capture (claimed dynamically)

## Key Risks

1. **IRQ Latency**: Core 1 DMA IRQ must be serviced every ~3.17us
   - Mitigation: IRQ handler in scratch RAM, DMA bus priority

2. **Frame Tearing**: Core 0 writes while Core 1 reads
   - Mitigation: Double-buffer in Phase 4, acceptable for Phase 3

3. **Clock Stability**: 126MHz required for HDMI timing
   - Mitigation: PIO capture works at any system clock

4. **GPIOBASE Hack**: Bank 1 access requires manual register writes
   - Mitigation: Proven working in main firmware
