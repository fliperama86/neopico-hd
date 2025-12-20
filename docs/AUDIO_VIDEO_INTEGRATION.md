# System Resource & Timing Analysis

## Step 1: System Inventory (RP2350 / Pico 2)

**RP2350 has 3 PIO blocks** (not 2 like RP2040), which resolves the SM conflict.

### GPIO Bank Access via gpio_base

Each PIO can only access 32 GPIOs relative to its `gpio_base` setting:
- `gpio_base=0`: Access GPIO 0-31
- `gpio_base=16`: Access GPIO 16-47

### Pin Assignments

| GPIO | Function | Bank |
| ---- | -------- | ---- |
| 0 | MVS PCLK | 0 |
| 1-15 | MVS RGB data | 0 |
| 22 | MVS CSYNC | 0 |
| 25-32 | DVI output | 1+ |
| 36 | I2S DAT | 1+ |
| 37 | I2S WS (LRCK) | 1+ |
| 38 | I2S BCK | 1+ |

### PIO Allocation Plan (Integrated)

| PIO  | gpio_base | SM  | Function |
| ---- | --------- | --- | -------- |
| PIO0 | 16 | 0 | DVI TMDS D0 |
| PIO0 | 16 | 1 | DVI TMDS D1 |
| PIO0 | 16 | 2 | DVI TMDS D2 |
| PIO0 | 16 | 3 | (free) |
| PIO1 | 0 | 0 | MVS sync detection |
| PIO1 | 0 | 1 | MVS pixel capture |
| PIO1 | 0 | 2 | (free) |
| PIO1 | 0 | 3 | (free) |
| **PIO2** | **16** | **0** | **I2S audio capture** |
| PIO2 | 16 | 1 | (free) |
| PIO2 | 16 | 2 | (free) |
| PIO2 | 16 | 3 | (free) |

**No conflict!** PIO2 (RP2350 only) handles I2S with gpio_base=16 for GPIO 36-38.

### DMA Channel Usage

| Component         | DMA Channels  | Transfer Pattern                                |
| ----------------- | ------------- | ----------------------------------------------- |
| DVI (PicoDVI)     | ~3-6 channels | Continuous TMDS streaming, IRQ-driven on Core 1 |
| I2S Audio Capture | 1 channel     | Continuous ring buffer from PIO2 RX FIFO        |
| MVS Video Capture | None          | Polled via pio_sm_get_blocking()                |

### CPU Core Assignment (Integrated)

| Core | Function | Notes |
| ---- | -------- | ----- |
| Core 0 | MVS capture loop | Blocking PIO reads, timing-critical |
| Core 0 | I2S capture polling | During vblank/safe periods only |
| Core 0 | Audio buffer filling | During vblank/safe periods only |
| Core 1 | DVI output | IRQ-driven DMA, scanline callback |

**Critical:** All Core 0 audio work must happen during safe periods (vsync wait, vblank lines, after frame capture) to avoid disrupting MVS capture timing.

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

### ~~Risk 1: PIO1 State Machine Conflict~~ ✅ RESOLVED

**Problem:** Video uses PIO1 SM0+SM1, Audio uses PIO1 SM0
**Solution:** Use PIO2 for I2S capture. RP2350 has 3 PIO blocks, eliminating the conflict entirely.

### ~~Risk 2: System Clock Mismatch~~ ✅ RESOLVED

**Problem:** Video-only ran at 126 MHz, audio test at 252 MHz.
**Solution:** Use 480p @ 252 MHz for everything. H_THRESHOLD stays at 288 (counts external PCLK edges, not system clock).

### ~~Risk 3: CPU Starvation During Video Capture~~ ✅ RESOLVED

**Problem:** Video capture uses blocking PIO reads - can't interleave audio during active capture.
**Solution:** Process audio only during safe periods (vsync wait, vblank lines, after frame). Timer interrupts cause video jitter - don't use them.

### Risk 4: DVI Timing Model Difference (MEDIUM)

**Problem:**

- Video uses scanline callback (Core 1 pulls from framebuffer)
- Audio test uses direct queue push (Core 0 pushes scanlines)

**Manifestation:** Different synchronization models may have different timing constraints.

### ~~Risk 5: gpio_base Conflict~~ ✅ RESOLVED

**Problem:** I2S needs gpio_base=16 for GPIO 36-38, MVS needs gpio_base=0 for GPIO 0-22.
**Solution:** Use separate PIOs: PIO1 (gpio_base=0) for MVS, PIO2 (gpio_base=16) for I2S.

### Ranked Risk Summary (Updated)

All major risks resolved:
- ✅ PIO conflict → Use PIO2 for I2S
- ✅ Clock mismatch → 480p @ 252 MHz
- ✅ CPU starvation → Vblank-driven audio
- ✅ gpio_base → Separate PIOs
- Risk 4 (DVI model) → Using scanline callback, works fine

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

1.  ~~PIO1 SM0 conflict~~ ✅ Use PIO2 (RP2350 has 3 PIOs)
2.  ~~Clock speed difference - must unify at 252 MHz for HDMI audio~~ ✅ Done
3.  ~~Video capture blocking model - limits where audio processing can run~~ ✅ Solved with vblank-driven approach

**Completed phases:**
- ✅ Phase 1: Clock unification (252 MHz / 480p)
- ✅ Phase 2: PIO allocation (PIO2 for I2S)
- ✅ Phase 3: I2S audio capture (verified working at 55.5 kHz)
- ✅ Phase 5: HDMI audio output (test tone working)

**Remaining phases:**
- Phase 4: Audio processing (DC filter, SRC) + connect I2S to HDMI output

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

---

## Key Finding: I2S Capture Timing Constraints

**Tested 2024-12-19:** I2S capture via PIO2 + DMA works, but polling has strict timing constraints.

### What Works

| Component | Configuration | Result |
| --------- | ------------- | ------ |
| PIO2 for I2S | gpio_base=16, SM0 | ✅ No conflict with video |
| DMA ring buffer | 2048 words (8KB) | ✅ Holds ~18ms of samples |
| Capture rate | 55554 Hz | ✅ Matches expected MVS rate |

### Critical Timing Constraints

**Problem:** Any I2S polling during active video capture causes visual glitches.

| Polling Location | Video Impact | I2S Rate |
| ---------------- | ------------ | -------- |
| During hblank (every 8 lines) | ❌ Horizontal trembling | 55 kHz |
| During vblank only | ⚠️ Minor instability | 38 kHz |
| After frame only | ✅ Stable video | 30 kHz |

**Root cause:** Even "fast" polling (just DMA→ring transfer) takes enough time to disrupt the tight 6 MHz pixel capture timing.

### USB Serial Causes Glitches

`printf()` over USB causes visible red line glitches. Disabled in production, use `#if 0` block for debugging.

### Buffer Sizing Solution

At 55.5 kHz stereo:
- ~925 stereo samples per frame (60 fps)
- ~1850 DMA words per frame

**Working configuration:**
| Buffer | Size | Purpose |
| ------ | ---- | ------- |
| DMA ring | 4096 words (16KB) | Holds ~2 frames from PIO |
| ap_ring | 2048 samples (16KB) | Holds ~2 frames for processing |

With these sizes, polling once per frame (after video capture) achieves full 55.5 kHz capture rate.

### Final Working Setup

- Poll I2S + drain ring **only after frame capture** (not during vblank)
- DMA buffer: 4096 words
- ap_ring: 2048 samples
- Result: 55553 Hz capture, stable 60 fps video

---

## Hardware Troubleshooting

### Missing Voices / Audio Channels

**Symptom:** Some audio channels (e.g., voices, specific instruments) are missing from the captured audio.

**Root cause:** Loose cartridge insertion on the MVS board.

**Solution:** Ensure the game cartridge is firmly seated in the MVS slot. The NEO-YSA2 audio chip receives data from the cartridge; a poor connection can cause channel dropouts.

**Note:** This is NOT a software issue - the I2S capture pipeline faithfully reproduces whatever the MVS outputs.

---

## Audio Noise Investigation - Key Findings

### Confirmed Findings

1. **Falling edge BCK sampling works better than rising edge**
   - Changed PIO to sample I2S DAT on BCK falling edge instead of rising
   - Significantly reduced noise when video cables are connected
   - Root cause: Rising edge timing affected by video signal coupling

2. **Proper frame sync eliminates random noise variation**
   - Added robust sync: wait for complete WS high→low→high→low cycle before capturing
   - Without this, noise levels varied randomly with each Pico reset
   - Now consistently aligned to frame boundary

3. **33pF cap on PCLK (Pico side) removes 99% of noise/crackling**
   - Cap placed close to Pico GPIO, connected to Pico GND
   - Slows down 6MHz clock edges, reducing coupling into I2S
   - Trade-off: Causes 1-pixel horizontal instability in video capture

4. **Noise only appears when video cables are connected to Pico**
   - Disconnecting video cables = completely clean audio
   - This is true even in audio_pipeline_test (no video capture code)
   - Scope shows clean I2S waveforms - noise is internal coupling, not signal integrity

5. **Analog MVS audio output is clean**
   - The noise is specific to I2S digital path when video GPIOs are loaded
   - Not a problem with the MVS audio generation itself

6. **CAV reference implementation differences**
   - CAV: Rising edge sampling, 16-bit capture, FIR filter
   - CAV: FPGA mounted directly on MVS with short traces
   - We have: Falling edge, 24-bit capture (extract 16), IIR filters, jumper wires

7. **Power supply concern**
   - LED on Pico dims significantly when everything is connected
   - Spotpear (DVI) draws from Pico's 3V3 rail
   - Pico's internal regulator may be stressed

### Open Questions

1. **Why does main firmware sound "compressed" vs audio_pipeline_test?**
   - audio_pipeline_test polls audio multiple times per scanline
   - Main firmware only polls during vblank/after frame capture
   - May be buffer overflow, irregular processing, or SRC artifacts

2. **Optimal cap value for PCLK?**
   - 33pF: Great audio, but causes video instability
   - Need to test 15-22pF for better balance
   - Alternative: Caps on data lines only, not PCLK

3. **Would series resistors work better than caps?**
   - 33-100Ω resistor limits current spikes without slowing edges as much
   - Could combine with smaller cap for RC filter with defined cutoff

4. **Is 3V3 rail droop contributing to noise?**
   - Unstable power = unstable GPIO thresholds
   - May need external 3V3 regulator for final design

5. **Can main firmware audio processing be improved?**
   - More frequent polling during safe windows?
   - Larger buffers to handle bursty processing?
   - Different SRC approach?
