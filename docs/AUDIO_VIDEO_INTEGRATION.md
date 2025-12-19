# System Resource & Timing Analysis

## Step 1: System Inventory

PIO State Machine Allocation

| PIO  | SM  | Usage (main.c - Video Only)      | Usage (audio_pipeline_test - Audio Only) |
| ---- | --- | -------------------------------- | ---------------------------------------- |
| PIO0 | 0   | DVI TMDS D0 serializer           | DVI TMDS D0 serializer                   |
| PIO0 | 1   | DVI TMDS D1 serializer           | DVI TMDS D1 serializer                   |
| PIO0 | 2   | DVI TMDS D2 serializer           | DVI TMDS D2 serializer                   |
| PIO0 | 3   | (free)                           | (free)                                   |
| PIO1 | 0   | MVS sync detection (mvs_sync_4a) | I2S audio capture                        |
| PIO1 | 1   | MVS pixel capture                | (free)                                   |
| PIO1 | 2   | (free)                           | (free)                                   |
| PIO1 | 3   | (free)                           | (free)                                   |

**CRITICAL CONFLICT:** Both video and audio want PIO1 SM0.

### DMA Channel Usage

| Component         | DMA Channels  | Transfer Pattern                                |
| ----------------- | ------------- | ----------------------------------------------- |
| DVI (PicoDVI)     | ~3-6 channels | Continuous TMDS streaming, IRQ-driven on Core 1 |
| I2S Audio Capture | 1 channel     | Continuous ring buffer from PIO1 RX FIFO        |
| MVS Video Capture | None          | Polled via pio_sm_get_blocking()                |

### Clock Domains

| Clock           | Frequency                              | Source                 |
| --------------- | -------------------------------------- | ---------------------- |
| System Clock    | 126 MHz (video) / 252 MHz (audio test) | PLL, set by DVI timing |
| MVS PCLK        | 6 MHz                                  | External (MVS board)   |
| MVS CSYNC       | ~15.6 kHz (line rate)                  | External (MVS board)   |
| I2S BCK         | 2.66 MHz                               | External (MVS board)   |
| I2S WS          | 55.5 kHz                               | External (MVS board)   |
| DVI Pixel Clock | 25.2 MHz (480p) / 12.6 MHz (240p)      | Derived from bit clock |

**CRITICAL DIFFERENCE:**

- Video-only runs at 126 MHz system clock
- Audio test runs at 252 MHz system clock

### IRQ Sources

| IRQ              | Handler            | Core   | Frequency                |
| ---------------- | ------------------ | ------ | ------------------------ |
| DMA_IRQ_0        | DVI scanline/TMDS  | Core 1 | ~31.5 kHz (per DVI line) |
| (none for video) | -                  | -      | -                        |
| (none for audio) | Audio uses polling | -      | -                        |

### Memory Buffers

| Buffer                    | Size                      | Location        | Lifetime        |
| ------------------------- | ------------------------- | --------------- | --------------- |
| g_framebuf (video)        | 320×240×2 = 153,600 bytes | Static          | Permanent       |
| g_framebuf (audio test)   | 320×232×2 = 148,480 bytes | Static          | Permanent       |
| scanline_buf (audio test) | 320×2×2 = 1,280 bytes     | Static          | Double-buffered |
| g_dma_buffer (I2S)        | 512×4 = 2,048 bytes       | Static, aligned | Ring buffer     |
| hdmi_audio_buffer         | 256×4 = 1,024 bytes       | Static          | Ring buffer     |
| ap_ring (capture)         | 512×4 = 2,048 bytes       | Static          | Ring buffer     |

### CPU Core Assignment

| Core   | Video-Only                            | Audio Test                          |
| ------ | ------------------------------------- | ----------------------------------- |
| Core 0 | MVS capture loop (blocking PIO reads) | Scanline generation + audio polling |
| Core 1 | DVI output (IRQ-driven DMA)           | DVI output (IRQ-driven DMA)         |

---

## Step 2: Timing & Deadline Analysis

### Video Pipeline Deadlines

| Operation                | Deadline                          | Type | Consequence of Miss            |
| ------------------------ | --------------------------------- | ---- | ------------------------------ |
| Read pixel from PIO FIFO | ~166 ns (6 MHz pixel clock)       | Hard | FIFO overflow, pixel loss      |
| Complete line capture    | 64 µs (MVS line period)           | Hard | Line desync, visual corruption |
| Complete frame capture   | 16.9 ms (59.19 Hz)                | Soft | Frame drop, but recoverable    |
| DVI scanline callback    | ~63.5 µs (480p) / ~79.4 µs (240p) | Hard | Display corruption             |

**Video capture is timing-critical:** The `pio_sm_get_blocking()` calls in the capture loop MUST keep up with the 6 MHz pixel clock. The PIO FIFO (8 words with join) provides ~1.3 µs of slack per line.

### Audio Pipeline Deadlines

| Operation                | Deadline                      | Type | Consequence of Miss              |
| ------------------------ | ----------------------------- | ---- | -------------------------------- |
| DMA buffer consumption   | ~18 µs (55.5 kHz sample rate) | Soft | DMA ring wrap-around handles it  |
| audio_pipeline_process() | 18 µs between calls           | Soft | Increased latency, buffer growth |
| HDMI audio ring write    | ~20.8 µs (48 kHz output rate) | Soft | TX drops (audible glitches)      |

**Audio is more forgiving: DMA handles capture autonomously. Polling just needs to keep up with average throughput, not instantaneous.**

### Latency-Sensitive vs Throughput-Sensitive

| Operation          | Sensitivity                                     |
| ------------------ | ----------------------------------------------- |
| MVS pixel capture  | Latency - blocking reads with no DMA            |
| MVS sync detection | Latency - must detect vsync edges promptly      |
| DVI output         | Latency - IRQ-driven, tight timing              |
| I2S capture        | Throughput - DMA buffered, tolerant of jitter   |
| Audio SRC          | Throughput - batch processing OK                |
| HDMI audio ring    | Throughput - 256-sample buffer absorbs variance |

---

## Step 3: Contention & Risk Analysis

### Risk 1: PIO1 State Machine Conflict (CRITICAL)

**Problem:** Video uses PIO1 SM0+SM1, Audio uses PIO1 SM0
Manifestation: Cannot run both simultaneously with current code
Mitigation: Audio must use PIO1 SM2 or SM3, OR use PIO0 SM3

### Risk 2: System Clock Mismatch (HIGH)

**Problem:**

- Video-only: 126 MHz (custom 240p timing)
- Audio test: 252 MHz (480p timing with HDMI audio)

**Manifestation:**

- At 126 MHz, the `H_THRESHOLD` calculation changes
- PIO timing for audio capture may be affected
- HDMI audio requires specific blank settings tied to 480p timing

**Integration choice required:** Must pick one timing mode and adapt.

### Risk 3: CPU Starvation During Video Capture (HIGH)

**Problem:** Video capture loop uses `pio_sm_get_blocking()` in tight loops:

```C
for (int x = 0; x < FRAME_WIDTH; x += 2) {
  uint32_t word = pio_sm_get_blocking(pio_mvs, sm_pixel); // Blocks!
  convert_and_store_pixels(word, &dst[x]);
}
```

This loop runs for 224 lines × 160 words = 35,840 blocking reads per frame.

**Manifestation:** If we `interleave audio_pipeline_process()` calls:

- Each audio poll takes ~5-20 µs (depending on samples available)
- 192 words per line × ~166 ns = 32 µs per line
- Adding audio processing could cause FIFO overflow

**Quantified risk:** Audio processing must complete in < 1.3 µs (FIFO slack) to avoid pixel loss.

### Risk 4: DVI Timing Model Difference (MEDIUM)

**Problem:**

- Video uses scanline callback (Core 1 pulls from framebuffer)
- Audio test uses direct queue push (Core 0 pushes scanlines)

**Manifestation:** Different synchronization models may have different timing constraints.

### Risk 5: gpio_base Conflict (MEDIUM)

**Problem:** I2S capture sets `pio_set_gpio_base(pio1, 16)` for Bank 1 access. Video capture uses Bank 0 pins on PIO1.

**Manifestation:** If gpio_base is set wrong, PIO1 won't see the correct GPIO pins.

### Ranked Risk Summary

1.  PIO1 SM conflict - Cannot proceed without fixing
2.  System clock mismatch - Must choose timing mode
3.  CPU starvation - Video blocking reads vs audio polling
4.  DVI timing model - Different approaches in each firmware
5.  gpio_base - Must be set correctly for each PIO user

---

## Step 4: Integration Strategy (Baby Steps)

### Phase 0: Baseline Measurements (Before Any Integration)

- Measure video-only loop timing with GPIO toggles
- Measure audio-only loop timing with GPIO toggles
- Confirm both work independently at 252 MHz (audio's clock)

### Phase 1: Clock Unification

**Goal:** Run video capture at 252 MHz instead of 126 MHz

**Changes:**

- Switch main.c to use dvi_timing_640x480p_60hz
- Adjust `FRAME_HEIGHT` to 232 (or 240 without audio initially)
- Update `H_THRESHOLD` for higher clock rate
- Test video capture still works

Success signal: Video displays correctly at 252 MHz
Failure signal: Vsync detection fails, display corrupt

### Phase 2: PIO Allocation Fix

**Goal:** Assign non-conflicting PIO state machines

Proposed allocation:

| PIO  | SM  | Function    |
| ---- | --- | ----------- |
| PIO0 | 0-2 | DVI TMDS    |
| PIO0 | 3   | (free)      |
| PIO1 | 0   | MVS sync    |
| PIO1 | 1   | MVS pixel   |
| PIO1 | 2   | I2S capture |
| PIO1 | 3   | (free)      |

**Changes:**

- Modify `audio_pipeline_config` to use SM2
- Ensure `gpio_base` is handled correctly (may need to be set before BOTH programs load)

Success signal: Both PIO programs load without error
Failure signal:`pio_add_program` or `pio_claim_unused_sm` fails

### Phase 3: Audio Capture Only (No HDMI Audio Output)

**Goal:** Run audio capture DMA alongside video, but don't output to HDMI yet

**Changes:**

- Add audio pipeline init to main.c
- Start audio capture
- Log capture rate, verify 55.5 kHz
- Do NOT call audio_output_callback yet

Success signal: Cap:55552Hz in serial output, video still works
Failure signal: Video corruption, audio rate wrong, crashes

Phase 4: Add Audio Processing (No HDMI Output)

**Goal:** Call audio_pipeline_process() but discard samples

**Changes:**

- Add processing calls at strategic points (between frames, during vblank)
- Measure processing time with GPIO toggles

Success signal: Video still works, processing completes in reasonable time
Failure signal: Video stutters, FIFO overflows

Phase 5: Enable HDMI Audio

**Goal:** Full integration with HDMI audio output

**Changes:**

- Enable blank_settings
- Configure HDMI audio ring buffer
- Connect audio_output_callback

Success signal: Audio plays through HDMI, video displays correctly
Failure signal: No signal, audio glitches, video scrolls

---

## Step 5: Measurement Plan

### Measurement 1: Video Capture Line Timing

What: Time to capture one video line (192 words)
How: Toggle GPIO at line start/end, measure with logic analyzer
Expected: ~32 µs (192 × 166 ns)
Hypothesis confirmed if: Consistent 32 µs per line
Hypothesis falsified if: Variable timing, > 64 µs

### Measurement 2: Audio Processing Duration

What: Time spent in audio_pipeline_process()
How: GPIO toggle around the call (already in code as profile_audio_process)
Expected: 5-20 µs depending on samples available
Hypothesis confirmed if: Processing fits in vblank gaps
Hypothesis falsified if: Processing > 30 µs

### Measurement 3: Video FIFO Level

What: How full is the pixel capture FIFO?
How: pio_sm_get_rx_fifo_level() periodically, log max
Expected: Should rarely exceed 4 (half of 8)
Hypothesis confirmed if: FIFO level stable
Hypothesis falsified if: FIFO hits 8 (overflow imminent)

### Measurement 4: Frame Timing Consistency

What: Does the frame rate stay at 59.19 fps?
How: Existing FPS counter in main loop
Expected: Consistent 59 fps
Hypothesis confirmed if: FPS stable after integration
Hypothesis falsified if: FPS drops or varies

### Measurement 5: Audio Sample Rate Stability

What: Is audio capturing at 55.5 kHz consistently?
How: Existing measured_rate in i2s_capture.c
Expected: 55552 Hz ± 50 Hz
Hypothesis confirmed if: Rate stable during video capture
Hypothesis falsified if: Rate drops or varies wildly

---

## Summary

**Primary integration blockers:**

1.  ~~PIO1 SM0 conflict - must reassign audio to SM2~~ (still needed for I2S capture)
2.  ~~Clock speed difference - must unify at 252 MHz for HDMI audio~~ ✅ Done
3.  ~~Video capture blocking model - limits where audio processing can run~~ ✅ Solved with vblank-driven approach

**Completed phases:**
- ✅ Phase 1: Clock unification (252 MHz / 480p)
- ✅ Phase 5: HDMI audio output (test tone working)

**Remaining phases:**
- Phase 2: PIO SM allocation for I2S capture
- Phase 3: I2S audio capture
- Phase 4: Audio processing (DC filter, SRC)

Key insight: Video capture's blocking nature means audio processing CANNOT be interleaved during active line capture. It must happen during vertical blanking (V_SKIP_LINES period) and between frames.

---

## Key Finding: 480p Required for HDMI Audio

**Tested 2024-12-19:** HDMI audio does NOT work reliably at 240p (126 MHz bit clock).

| Mode | Video | Audio | Result |
| ---- | ----- | ----- | ------ |
| 480p @ 252 MHz | Stable | Working | **Use this** |
| 240p @ 126 MHz | Flickering | Works but corrupts video | Not viable |

**Root cause:** At 126 MHz, the CPU/DMA cannot keep up with TMDS encoding + data island encoding simultaneously. The data islands (which carry audio) overrun into active video, causing flickering.

**Decision:** Use 480p (252 MHz) for audio integration. The Tink 4K handles upscaling well.

**Impact on main firmware:**
- Must switch from 240p to 480p timing
- System clock: 252 MHz instead of 126 MHz
- H_THRESHOLD stays at 288 (counts external PCLK edges, not system clock)

---

## Key Finding: Scanline Callback Audio Integration

**Tested 2024-12-19:** Successfully integrated HDMI audio with MVS video capture using scanline callback model.

### What Works

| Component | Approach | Result |
| --------- | -------- | ------ |
| Video timing | 480p @ 252 MHz | Stable |
| blank_settings | **Not needed** | Audio works without them |
| Audio filling | Vblank-driven (no timer) | Clean audio |
| Buffer size | 1024 samples | Smooths irregular timing |

### Critical Discovery: No Timer Interrupts

**Problem:** Using `add_repeating_timer_ms()` for audio caused horizontal video trembling.

**Root cause:** Timer interrupts Core 0 during MVS capture. The `pio_sm_get_blocking()` calls are timing-critical - any interruption causes PIO FIFO timing jitter.

**Solution:** Fill audio buffer during safe periods only:
1. During `wait_for_vsync()` when PIO FIFO is empty
2. Between V_SKIP_LINES (vertical blanking)
3. After frame capture completes

### Audio Buffer Sizing

- 256 samples: Audio oscillates (buffer runs dry between fills)
- 1024 samples: Clean audio (enough headroom for irregular fill timing)

At 48kHz output and 60fps, need ~800 samples/frame. Larger buffer absorbs timing variance.

### Code Pattern

```c
// Fill during vsync wait (in wait_for_vsync)
if (pio_sm_is_rx_fifo_empty(pio, sm_sync)) {
    fill_audio_buffer();
    continue;
}

// Fill between vblank lines
for (int skip_line = 0; skip_line < V_SKIP_LINES; skip_line++) {
    // ... consume line ...
    fill_audio_buffer();
}

// Fill after frame capture
pio_sm_set_enabled(pio_mvs, sm_pixel, false);
fill_audio_buffer();
```

### blank_settings Not Required

Originally thought `blank_settings` were needed for HDMI audio data islands. Testing showed:
- Scanline callback model works without blank_settings
- Standard horizontal blanking provides enough space for data islands
- FRAME_HEIGHT stays at 240 (not 232)
