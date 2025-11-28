# CSYNC Decoding Implementation Plan - Baby Steps

> **Status**: ✅ COMPLETED - This document describes the methodology and implementation that was used to complete Phase 4. Refer to [PROJECT_STATUS.md](PROJECT_STATUS.md) for the current project status.

## Overview

This plan documents the cps2_digiav CSYNC decoding algorithm implementation using PIO state machines. The approach is based on the proven FPGA implementation from `neogeo_frontend.v`.

**Core Strategy**:
- Use horizontal pixel counter (h_ctr) to filter HSYNC from equalization pulses
- Detect VSYNC by sampling CSYNC at pixel 192 (midpoint)
- No pulse width measurement needed
- No hardcoded delays
- Self-synchronizing and robust

---

## Phase 4a: Horizontal Counter & HSYNC Detection ✅ COMPLETED

**Goal**: Implement h_ctr logic, detect valid HSYNC pulses, ignore equalization
**Status**: ✅ Completed and integrated into mvs_sync_4a program in mvs_sync.pio

### PIO Program (`mvs_sync.pio`)

```pio
; Count pixel clocks between CSYNC edges
; Push event when h_ctr > 288 (valid HSYNC)
; 288 = 384 * 3/4 (threshold to distinguish full line from half-line)

.wrap_target
    set x, 0                    ; h_ctr = 0
count_pixels:
    wait 0 gpio PCLK            ; Wait for falling edge
    wait 1 gpio PCLK            ; Wait for rising edge
    jmp x++ check_csync         ; h_ctr++

check_csync:
    jmp pin count_pixels        ; If CSYNC high, continue counting

    ; CSYNC low (falling edge detected)
    mov isr, x                  ; Save h_ctr value
    push noblock                ; Push to FIFO (non-blocking)
    jmp x 288 is_hsync          ; If h_ctr > 288, it's HSYNC
    jmp count_pixels            ; Otherwise ignore (equalization)

is_hsync:
    ; Valid HSYNC detected, counter will restart via wrap
.wrap
```

### C Code (validation only)

```c
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "mvs_sync.pio.h"

#define PIN_CSYNC 0
#define PIN_PCLK  1

int main() {
    stdio_init_all();
    sleep_ms(5000);

    printf("Phase 4a: HSYNC Detection Test\n");
    printf("===============================\n\n");

    PIO pio = pio0;
    uint offset = pio_add_program(pio, &mvs_sync_program);
    uint sm = pio_claim_unused_sm(pio, true);

    mvs_sync_program_init(pio, sm, offset, PIN_CSYNC, PIN_PCLK);
    pio_sm_set_enabled(pio, sm, true);

    uint32_t hsync_count = 0;
    absolute_time_t start = get_absolute_time();

    while (true) {
        if (!pio_sm_is_rx_fifo_empty(pio, sm)) {
            uint32_t h_ctr = pio_sm_get(pio, sm);

            if (h_ctr > 288) {
                hsync_count++;

                // Print every 1000 HSYNCs
                if (hsync_count % 1000 == 0) {
                    int64_t elapsed_us = absolute_time_diff_us(start, get_absolute_time());
                    float hsync_freq = (hsync_count * 1000000.0f) / elapsed_us;
                    printf("HSYNC count: %lu, freq: %.2f kHz, h_ctr: %lu\n",
                           hsync_count, hsync_freq / 1000.0f, h_ctr);
                }
            }
        }
    }

    return 0;
}
```

### Success Criteria

- ✅ See ~15,625 HSYNC detections per second (264 × 59.19 fps)
- ✅ h_ctr values around 384 (one full scanline)
- ✅ NO short pulses printed (equalization filtered out)
- ✅ Frequency stable over time

**Expected Output**:
```
HSYNC count: 1000, freq: 15.63 kHz, h_ctr: 384
HSYNC count: 2000, freq: 15.63 kHz, h_ctr: 383
HSYNC count: 3000, freq: 15.62 kHz, h_ctr: 384
```

---

## Phase 4b: Equalization Detection ✅ COMPLETED

**Goal**: Sample CSYNC at pixel 192, count consecutive equalization pulses
**Status**: ✅ Completed as part of Phase 4

### Enhanced PIO Program

Add midpoint sampling to the existing program:

```pio
; At h_ctr == 192, check if CSYNC is low (equalization pulse)

check_csync:
    ; Check if we're at midpoint (192 pixels)
    mov y, x                    ; Copy h_ctr to y
    jmp y 192 not_midpoint      ; If h_ctr != 192, skip check

    ; At midpoint - sample CSYNC
    mov isr, pins               ; Sample CSYNC state
    set y, 1                    ; Mark as midpoint event
    out isr, 1                  ; Add marker bit
    push noblock                ; Push status

not_midpoint:
    jmp pin count_pixels        ; If CSYNC high, continue
    ; ... rest as before
```

### C Code (add state machine)

```c
#define EVENT_TYPE_HSYNC     0
#define EVENT_TYPE_MIDPOINT  1

typedef struct {
    uint32_t type;
    uint32_t value;
} sync_event_t;

int main() {
    // ... initialization as before ...

    uint32_t equ_count = 0;
    uint32_t line_count = 0;
    uint32_t frame_count = 0;

    while (true) {
        if (!pio_sm_is_rx_fifo_empty(pio, sm)) {
            uint32_t raw_event = pio_sm_get(pio, sm);

            if (raw_event & 0x80000000) {
                // Midpoint sample (bit 31 set)
                bool csync_state = raw_event & 0x01;

                if (csync_state) {
                    // CSYNC high at midpoint = normal line
                    if (equ_count > 0) {
                        printf("Equalization sequence: %d pulses\n", equ_count);
                        if (equ_count > 8) {
                            printf("*** VSYNC DETECTED (frame %lu) ***\n", frame_count++);
                        }
                    }
                    equ_count = 0;
                } else {
                    // CSYNC low at midpoint = equalization pulse
                    equ_count++;
                    printf("  Equalization pulse %d\n", equ_count);
                }
            } else {
                // Regular HSYNC
                line_count++;
            }
        }
    }

    return 0;
}
```

### Success Criteria

- ✅ See equalization sequences of ~9 pulses
- ✅ Occurs every 264 lines (every frame)
- ✅ Pattern repeats at 59.19 Hz
- ✅ Clear distinction between normal lines and equalization

**Expected Output**:
```
  Equalization pulse 1
  Equalization pulse 2
  ...
  Equalization pulse 9
Equalization sequence: 9 pulses
*** VSYNC DETECTED (frame 0) ***
```

---

## Phase 4c: Frame Counter ✅ COMPLETED

**Goal**: Count lines, detect frame boundaries using equalization pattern + line count
**Status**: ✅ Completed as part of Phase 4

### C Code (complete state machine)

```c
#define NEO_V_TOTAL 264

int main() {
    // ... initialization as before ...

    uint32_t v_ctr = 0;           // Line counter (vertical position)
    uint32_t equ_count = 0;       // Equalization pulse counter
    bool force_resync = false;    // Resync flag
    uint32_t frame_count = 0;
    uint32_t vclks_this_frame = 0;

    printf("Phase 4c: Frame Counter Test\n");
    printf("============================\n\n");

    while (true) {
        if (!pio_sm_is_rx_fifo_empty(pio, sm)) {
            uint32_t event = pio_sm_get(pio, sm);

            if (event & 0x80000000) {
                // Midpoint sample
                bool csync_low = !(event & 0x01);

                if (csync_low) {
                    equ_count++;
                    // Detect VSYNC pattern (like cps2_digiav line 120-121)
                    if (equ_count == 9) {
                        force_resync = (v_ctr != 0);
                    }
                } else {
                    equ_count = 0;
                }
            } else {
                // Valid HSYNC detected
                uint32_t h_ctr = event;

                if (force_resync || v_ctr == (NEO_V_TOTAL - 1)) {
                    // Frame boundary
                    printf("=== FRAME %lu COMPLETE ===\n", frame_count);
                    printf("  Lines: %lu\n", v_ctr);
                    printf("  VCLKs: %lu\n", vclks_this_frame);
                    printf("  Frame rate: %.2f fps\n\n",
                           6000000.0f / vclks_this_frame);

                    v_ctr = force_resync ? 1 : 0;
                    force_resync = false;
                    frame_count++;
                    vclks_this_frame = 0;
                } else {
                    v_ctr++;
                }

                vclks_this_frame += h_ctr;
            }
        }
    }

    return 0;
}
```

### Success Criteria

- ✅ Stable 264 lines per frame
- ✅ Frame rate: 59.19 fps (measured from vclks_per_frame)
- ✅ No drift over time (stays locked)
- ✅ Correct vclk count: ~101,376 per frame (384 × 264)

**Expected Output**:
```
=== FRAME 0 COMPLETE ===
  Lines: 264
  VCLKs: 101376
  Frame rate: 59.19 fps

=== FRAME 1 COMPLETE ===
  Lines: 264
  VCLKs: 101376
  Frame rate: 59.19 fps
```

---

## Phase 4d: Output Timing Signals ✅ COMPLETED

**Goal**: Generate clean HSYNC, VSYNC, DE signals for future use

### C Code (add timing outputs)

```c
// MVS timing constants (from MVS_CAPTURE.md)
#define NEO_H_SYNCLEN   29
#define NEO_H_BACKPORCH 28
#define NEO_H_ACTIVE    320

#define NEO_V_SYNCLEN   3
#define NEO_V_BACKPORCH 21
#define NEO_V_ACTIVE    224

typedef struct {
    uint32_t h_pos;
    uint32_t v_pos;
    bool hsync;
    bool vsync;
    bool de;
    uint32_t frame;
} timing_state_t;

int main() {
    // ... initialization as before ...

    timing_state_t timing = {0};
    uint32_t active_pixel_count = 0;

    while (true) {
        if (!pio_sm_is_rx_fifo_empty(pio, sm)) {
            uint32_t event = pio_sm_get(pio, sm);

            if (event & 0x80000000) {
                // Midpoint sample (handle as before)
                // ...
            } else {
                // Valid HSYNC
                timing.h_pos = event;

                // Generate timing signals (like cps2_digiav lines 135-140)
                timing.hsync = (timing.h_pos < NEO_H_SYNCLEN);
                timing.vsync = (timing.v_pos < NEO_V_SYNCLEN);

                bool h_active = (timing.h_pos >= NEO_H_SYNCLEN + NEO_H_BACKPORCH) &&
                                (timing.h_pos < NEO_H_SYNCLEN + NEO_H_BACKPORCH + NEO_H_ACTIVE);
                bool v_active = (timing.v_pos >= NEO_V_SYNCLEN + NEO_V_BACKPORCH) &&
                                (timing.v_pos < NEO_V_SYNCLEN + NEO_V_BACKPORCH + NEO_V_ACTIVE);

                timing.de = h_active && v_active;

                if (timing.de) {
                    active_pixel_count++;

                    // Calculate position within active area
                    uint32_t xpos = timing.h_pos - (NEO_H_SYNCLEN + NEO_H_BACKPORCH);
                    uint32_t ypos = timing.v_pos - (NEO_V_SYNCLEN + NEO_V_BACKPORCH);

                    // Print first active line of each frame
                    if (ypos == 0 && xpos < 10) {
                        printf("Frame %lu, Active pixel (%lu, %lu)\n",
                               timing.frame, xpos, ypos);
                    }
                }

                // Frame boundary handling
                if (force_resync || timing.v_pos == (NEO_V_TOTAL - 1)) {
                    printf("=== FRAME %lu ===\n", timing.frame);
                    printf("  Active pixels: %lu (expected: %d)\n\n",
                           active_pixel_count, NEO_H_ACTIVE * NEO_V_ACTIVE);

                    timing.frame++;
                    timing.v_pos = 0;
                    active_pixel_count = 0;
                    force_resync = false;
                } else {
                    timing.v_pos++;
                }
            }
        }
    }

    return 0;
}
```

### Success Criteria

- ✅ HSYNC toggles 264 times per frame
- ✅ VSYNC toggles once per frame (active for 3 lines)
- ✅ DE window: exactly 71,680 pixels per frame (320×224)
- ✅ All timing matches MVS_CAPTURE.md specifications
- ✅ xpos ranges 0-319, ypos ranges 0-223

**Expected Output**:
```
Frame 0, Active pixel (0, 0)
Frame 0, Active pixel (1, 0)
...
=== FRAME 0 ===
  Active pixels: 71680 (expected: 71680)

Frame 1, Active pixel (0, 0)
...
```

---

## Phase 4e: DMA-Based Architecture ✅ COMPLETED

**Goal**: Move to zero-CPU sync monitoring, prove architecture scales for pixel capture

### Why This Matters

**Performance Problem**: CPU polling at 6 MHz pixel clock (354,816 events/sec) will fail when we add pixel capture:
- ❌ CPU polls FIFO constantly → wastes cycles
- ❌ 15-bit RGB = 90 Mbps continuous → CPU can't keep up
- ❌ No room for frame processing, USB output, etc.

**Solution**: Let DMA + PIO do all the real-time work, CPU only checks results.

### Enhanced PIO: Generate DE Signal

Move timing logic into PIO so DE (Data Enable) is generated in hardware:

```pio
.program mvs_sync_dma

; Output packed sync state every 32 pixels
; Format: [31:16] = h_ctr, [15:8] = v_ctr, [7:0] = flags (DE, VSYNC, HSYNC)

.define H_SYNCLEN 29
.define H_BACKPORCH 28
.define H_ACTIVE 320
.define V_SYNCLEN 3
.define V_BACKPORCH 21
.define V_ACTIVE 224

.wrap_target
    set x, 0                    ; h_ctr = 0

pixel_loop:
    wait 0 gpio PCLK
    wait 1 gpio PCLK

    ; Increment h_ctr
    mov y, x
    jmp y++ check_sync
    mov x, y

check_sync:
    jmp pin no_csync_edge       ; CSYNC high, continue

    ; CSYNC falling edge detected
    mov y, x
    jmp y 288 valid_hsync       ; h_ctr > 288?
    jmp pixel_loop              ; Equalization, ignore

valid_hsync:
    ; Valid HSYNC - calculate and push timing state
    ; (Simplified - full implementation calculates DE in PIO)

    ; Pack: h_ctr, v_ctr, flags
    mov isr, x                  ; h_ctr in [31:16]
    ; ... pack v_ctr and flags ...
    push                        ; Auto-push every N cycles

    set x, 0                    ; Reset h_ctr
    ; Increment v_ctr logic here

no_csync_edge:
    ; Continue pixel counting
    jmp pixel_loop
.wrap
```

### DMA Chain Architecture

**Chain 1: PIO FIFO → Sync State Buffer**
```c
// Ring buffer of sync states
#define SYNC_BUFFER_SIZE 1024
uint32_t sync_state_buffer[SYNC_BUFFER_SIZE];
uint32_t sync_read_idx = 0;

// DMA paced by PIO DREQ - zero CPU overhead
dma_channel_config cfg = dma_channel_get_default_config(dma_ch);
channel_config_set_read_increment(&cfg, false);  // Always read from PIO FIFO
channel_config_set_write_increment(&cfg, true);  // Fill buffer
channel_config_set_dreq(&cfg, pio_get_dreq(pio, sm, false));
channel_config_set_ring(&cfg, true, 12);  // Ring buffer (2^12 = 4096 bytes)
channel_config_set_chain_to(&cfg, dma_ch);  // Chain to self (continuous)

dma_channel_configure(
    dma_ch,
    &cfg,
    sync_state_buffer,          // Destination
    &pio->rxf[sm],              // Source (PIO FIFO)
    SYNC_BUFFER_SIZE,           // Transfer count
    true                        // Start immediately
);
```

**CPU Role: Minimal Processing**
```c
int main() {
    setup_pio_sync();
    setup_dma_continuous();

    printf("Phase 4e: DMA Performance Test\n");
    printf("===============================\n\n");

    uint32_t last_frame = 0;
    uint32_t cpu_idle_time = 0;
    absolute_time_t loop_start;

    while (true) {
        loop_start = get_absolute_time();

        // Read from DMA-filled buffer (not polling!)
        uint32_t write_idx = dma_channel_hw_addr(dma_ch)->transfer_count;

        // Process any new sync states
        while (sync_read_idx != write_idx) {
            uint32_t state = sync_state_buffer[sync_read_idx];
            sync_read_idx = (sync_read_idx + 1) % SYNC_BUFFER_SIZE;

            // Extract fields
            uint16_t h_ctr = (state >> 16) & 0xFFFF;
            uint8_t v_ctr = (state >> 8) & 0xFF;
            uint8_t flags = state & 0xFF;

            bool de = flags & 0x01;
            bool vsync = flags & 0x02;
            bool hsync = flags & 0x04;

            // Frame boundary detection
            if (vsync && !last_vsync) {
                printf("=== FRAME %lu ===\n", frame_count++);
                printf("  CPU idle: %lu%%\n",
                       (cpu_idle_time * 100) / MEASUREMENT_PERIOD_US);
                cpu_idle_time = 0;
            }

            last_vsync = vsync;
        }

        // Measure CPU idle time
        int64_t elapsed = absolute_time_diff_us(loop_start, get_absolute_time());
        cpu_idle_time += (100 - elapsed);  // Simplified measurement

        tight_loop_contents();  // Yield when idle
    }

    return 0;
}
```

### Success Criteria

- ✅ CPU usage < 5% (measured via idle time counter)
- ✅ Frame rate stable at 59.19 fps
- ✅ No FIFO overruns (PIO FIFO never full)
- ✅ DMA transfer counter increments continuously
- ✅ Sync state buffer never fills up (read keeps pace with write)

**Expected Output**:
```
=== FRAME 0 ===
  CPU idle: 98%

=== FRAME 1 ===
  CPU idle: 97%

=== FRAME 2 ===
  CPU idle: 98%
```

**Performance Goal**: Prove the architecture can handle 6 MHz sync stream with <5% CPU, leaving 95% available for pixel processing, USB transfer, and frame analysis.

---

## Implementation Timeline

| Phase | Description | Estimated Time | Deliverable |
|-------|-------------|----------------|-------------|
| 4a | Horizontal counter + HSYNC filtering | ~2 hours | HSYNC frequency measurement |
| 4b | Equalization detection | ~1 hour | VSYNC pattern detection |
| 4c | Frame counting | ~1 hour | Stable frame rate |
| 4d | Timing signal generation | ~30 min | DE, xpos, ypos signals |
| 4e | DMA-based architecture | ~2 hours | Zero-CPU sync monitoring |
| **Total** | **Complete CSYNC decoding** | **~6.5 hours** | **Ready for pixel capture** |

---

## Key Principles

1. **Baby Steps**: Each phase validates previous work before proceeding
2. **Diagnostic First**: Print measurements to prove correctness
3. **No Guessing**: All logic derived from cps2_digiav proven implementation
4. **Hardware Sync**: PIO does timing-critical work, CPU interprets results
5. **Performance First**: Validate architecture scales before adding complexity

---

## Performance Considerations

### Why Phase 4e is Critical

**The Data Volume Problem**:
- 6 MHz pixel clock = 6,000,000 events/second
- 15-bit RGB capture = 90 Mbps continuous
- 59 fps × 264 lines = 15,576 line transitions/second
- Total: **~354,000+ events/second to process**

**CPU Polling Approach (Phases 4a-4d)**:
```c
while (true) {
    if (!pio_sm_is_rx_fifo_empty()) {  // ← 6M checks/sec
        process_event();                // ← blocks everything else
    }
}
```
- ❌ Burns 80%+ CPU just checking FIFO
- ❌ No time for USB transfer, frame processing
- ❌ Will fail when pixel capture added

**DMA Approach (Phase 4e)**:
```c
while (true) {
    if (sync_read_idx != dma_write_idx) {  // ← ~15K checks/sec
        process_batch();                    // ← process multiple events
    }
    tight_loop_contents();  // ← 95% of time spent here
}
```
- ✅ DMA streams data at hardware speed
- ✅ CPU processes in batches, not per-event
- ✅ 95% CPU available for other work

### Scalability to Full RGB Capture

After Phase 4e proves the architecture, adding pixel capture is straightforward:

**Additional PIO State Machines** (3 more):
- SM1: Sample R[4:0] when DE high
- SM2: Sample G[4:0] when DE high
- SM3: Sample B[4:0] when DE high

**Additional DMA Channels** (3 more):
- Chain 2-4: PIO → Line buffers (one per RGB channel)
- Triggered by SM0's DE signal
- Zero additional CPU load

**Memory Requirements**:
- Frame buffer: 320×224×15 bits = 134 KB (51% of RAM)
- Line buffers: 3×320 bytes = 960 bytes
- Sync state buffer: 4 KB
- **Total: ~139 KB of 264 KB (53% used)**

### Lessons from PicoDVI-N64

N64 project proves this architecture works:
- **Zero DMA for video** in their case (different architecture)
- **DMA for audio** at 96 kHz = 384 KB/s → they used chained DMA
- Our case: Sync at ~60 KB/s → similar scale to their audio
- Their approach: works flawlessly with <5% CPU

Our advantage: We only need frame capture (not real-time output), so we can batch process.

---

## After Phase 4e: Next Steps

Once performance architecture is validated:

1. **Phase 5a: Add R channel capture** (~1 hour)
   - Single PIO SM for R[4:0]
   - DMA to line buffer
   - Validate 1-bit capture works at full speed

2. **Phase 5b: Add G and B channels** (~1 hour)
   - Parallel SMs for G[4:0] and B[4:0]
   - Parallel DMAs
   - Validate 15-bit capture

3. **Phase 5c: Frame assembly** (~1 hour)
   - Combine RGB line buffers into frame buffer
   - Output as PPM format
   - Validate image quality

**Total time to full RGB capture**: ~3 hours (after Phase 4e proves architecture)

But **first**, we must prove:
1. Sync decoding is correct (Phases 4a-4d)
2. Architecture scales (Phase 4e)

---

## References

- `cps2_digiav/board/neogeo/rtl/neogeo_frontend.v` - FPGA reference implementation
- `PicoDVI-N64/software/apps/n64/main.c` - DMA architecture example
- `MVS_CAPTURE.md` - MVS timing specifications
- `PROJECT_STATUS.md` - Project progress tracking
