# Genlock Option (59 Hz vs 60 Hz Output)

This document describes the frame-rate mismatch between MVS input and HDMI output,
why it causes stutter, all approaches evaluated for genlock, RP2350 hardware
constraints, and the recommended hybrid architecture.

---

## 1. The Problem

- **MVS** runs at approximately **59.18 Hz** (Neo Geo; exact value depends on the
  board's crystal).
- **NeoPico-HD output** is fixed at **60 Hz** (system clock 126 MHz → 25.2 MHz
  pixel clock → standard 640×480 or 1280×240 timing).
- The ~0.82 Hz rate mismatch means we periodically **show the same input frame
  twice** (one frame held for two display refreshes) about every ~1.2 seconds.
- In smooth side-scrolling games this appears as a **slight stutter or judder**.

---

## 2. How cps2_digiav Handles It (Reference)

The CPS2_digiav reference project (in `~/Projects/references/cps2_digiav`) avoids
this by:

1. **Framelocked output rate** — The output pixel clock is derived from the
   **source** via a **Si5351C** programmable clock generator. The output refresh
   rate equals the source (e.g. 59.19 Hz for Neo Geo, 59.64 Hz for CPS2). No
   60 Hz conversion.

2. **V_STARTLINE** — On each input frame change, the FPGA repositions the output
   scanline counter to a computed line so input and output stay in phase.

3. **ASRC for audio** — Uses an asynchronous sample-rate converter (FIR
   interpolation filter) between input and output clock domains. No explicit
   feedback loop; the filter naturally tracks input/output rate differences.

4. **Result** — They output non-standard rates (~59.2–59.6 Hz) to the TV and rely
   on the display to accept them. Most modern TVs do.

NeoPico-HD cannot do mid-stream scanline repositioning (HSTX has no equivalent to
V_STARTLINE), and has no external clock generator. The strategies below work within
the RP2350's native hardware.

---

## 3. All Options Evaluated

| # | Option | Effort | Stutter | TV compat | Notes |
|---|--------|--------|---------|-----------|-------|
| 1 | Do nothing (60 Hz) | None | ~1.2s judder | Best | Current behaviour |
| 2 | Static PLL switch (59 Hz) | Low | Residual drift | Good | Match nominal rate, accept PLL granularity |
| 3 | Adaptive blanking | Medium | None (locked) | Good | Variable v_total tracks input, fixed pixel clock |
| 4 | **Hybrid: PLL + adaptive blanking** | **Medium** | **None** | **Good** | **Recommended** — PLL gets close, blanking tracks residual |
| 5 | FBDIV dithering | High | Jitter risk | Unknown | Alternate FBDIV values to average a frequency |
| 6 | External clock (Si5351) | Hardware | None | Good | Extra IC, board change, true hardware genlock |
| 7 | GPIN external input | N/A | — | — | **Not viable** — see §4.5 |
| 8 | Frame drop/repeat at 60 Hz | Low | Reduced | Best | Smarter scheduling, still visible |

**Recommended: Option 4** — Hybrid PLL + adaptive blanking. Gets exact lock with
no external hardware.

---

## 4. RP2350 Clock Hardware Constraints

### 4.1 PLL Structure (Datasheet §8.6)

Two PLLs: `pll_sys` and `pll_usb`.

```
Output = (XOSC / REFDIV) × FBDIV / (POSTDIV1 × POSTDIV2)
```

| Parameter | Range | Notes |
|-----------|-------|-------|
| XOSC | 12 MHz | Fixed crystal |
| REFDIV | 1–63 | Ref freq must be ≥ 5 MHz → REFDIV ≤ 2 |
| FBDIV | 16–320 | **Integer only** — no fractional-N |
| POSTDIV1 | 1–7 | Higher value = better efficiency |
| POSTDIV2 | 1–7 | |
| VCO | 750–1600 MHz | = (XOSC / REFDIV) × FBDIV |

**Current config** (126 MHz): REFDIV=1, FBDIV=105, POSTDIV1=5, POSTDIV2=2.
VCO = 12 × 105 = 1260 MHz. Output = 1260 / 10 = 126 MHz.

### 4.2 Frequency Resolution Per FBDIV Step

Each FBDIV increment changes VCO by `XOSC / REFDIV`:

| REFDIV | VCO step | Best POSTDIV | sys_clk step | pixel step (/5) | Frame rate step (480p) |
|--------|----------|--------------|--------------|-----------------|------------------------|
| 1 | 12 MHz | 10 | 1.2 MHz | 0.24 MHz | **~0.57 Hz** |
| 2 | 6 MHz | 7 | 0.857 MHz | 0.171 MHz | **~0.41 Hz** |

**0.41 Hz minimum step is far too coarse for continuous genlock tracking.** This is
the fundamental limitation — RP2350 has no fractional-N PLL.

### 4.3 Best PLL Settings for 59.18 Hz

Target: pixel_clock × h_total × v_total = 59.18 Hz.

With standard 480p blanking (h_total=800, v_total=525):
- pixel_clock = 59.18 × 420,000 = 24,855,600 Hz
- sys_clk = 24,855,600 × 5 = 124,278,000 Hz

**Best achievable with REFDIV=2, POSTDIV1=7, POSTDIV2=1:**

| FBDIV | VCO (MHz) | sys_clk (MHz) | pixel (MHz) | Refresh (Hz) |
|-------|-----------|---------------|-------------|--------------|
| 144 | 864 | 123.429 | 24.686 | 58.776 |
| **145** | **870** | **124.286** | **24.857** | **59.184** |
| 146 | 876 | 125.143 | 25.029 | 59.593 |
| 147 | 882 | 126.000 | 25.200 | 60.000 |

**FBDIV=145 gives 59.184 Hz** — only 0.004 Hz off from 59.18 Hz (0.007% error).
This is close enough that adaptive blanking needs to correct < 1 line per frame.

**PLL parameters for 59.184 Hz mode:**
```c
// REFDIV=2, FBDIV=145, POSTDIV1=7, POSTDIV2=1
// VCO = 6 × 145 = 870 MHz (within 750–1600 range)
// sys_clk = 870 / 7 = 124.2857 MHz
// pixel_clock = 124.2857 / 5 = 24.8571 MHz
// 480p: 24857143 / (800 × 525) = 59.184 Hz
set_sys_clock_pll(870000000, 7, 1);  // VCO=870MHz, PD1=7, PD2=1
```

### 4.4 clk_hstx Constraints

| Property | Value | Impact |
|----------|-------|--------|
| AUXSRC options | CLK_SYS, PLL_SYS, PLL_USB, GPIN0, GPIN1 | Can decouple from sys_clk |
| DIV register | Integer only (bits 17:16) | **No fractional divider** |
| CSR_CLKDIV | Integer divider inside HSTX | Currently 5 |

Unlike `clk_gpout0-3` which have 16.16 fractional dividers, clk_hstx is
**integer-only at every stage**. Fine-grained frequency tuning via clock division
is not possible.

### 4.5 GPIN (External Clock Input) — Not Viable

clk_hstx AUXSRC supports GPIN0/GPIN1, but:

- **GPIN max input**: 50 MHz
- **clk_hstx DIV**: Integer-only, can only divide **down** (not multiply up)
- **MVS PCLK**: 6 MHz — we need ~25 MHz pixel clock, can't multiply within RP2350
- **PLL ref input**: Tied to XOSC, not GPIN — cannot use GPIN as PLL reference
- **Jitter**: Raw external signal has no PLL cleanup; jitter goes straight to TMDS

To use GPIN for genlock, you'd need an external PLL (e.g. Si5351) to multiply
6 MHz → 126 MHz first. At that point, it's Option 6 (external clock).

### 4.6 Runtime FBDIV Changes

Writing `pll_sys->fbdiv_int` at runtime:

- PLL **loses lock** — `PLL_CS_LOCK` clears
- VCO frequency ramps (overshoot/undershoot during transition)
- Re-lock takes **~100–500 µs**
- During this window: clk_hstx is unstable → **TMDS garbage → TV may drop sync**
- Also changes sys_clk → affects Core 0 capture timing, audio, everything

**Possible safe window**: VSYNC blanking (~1.4 ms for 480p, 45 blanking lines).
PLL has time to re-lock before active video. But the coarse step size
(0.41+ Hz minimum) makes this useless for continuous tracking — only for initial
calibration or mode switching.

### 4.7 PLL_USB as Dedicated HSTX Source

clk_hstx can be sourced from `CLKSRC_PLL_USB` instead of `CLK_SYS`:

```
PLL_SYS → clk_sys   (126 MHz, fixed — Core 0 capture, audio, everything)
PLL_USB → clk_hstx  (variable — HSTX only)
```

**Pros:**
- Isolates HSTX clock changes from sys_clk
- No impact on Core 0 capture timing or audio pipeline
- Can reprogram PLL_USB independently during VSYNC blanking

**Cons:**
- Same coarse FBDIV resolution as PLL_SYS
- Loses USB functionality (acceptable — NeoPico-HD doesn't use USB at runtime)
- Still needs adaptive blanking for fine tracking

**This is the recommended approach for the PLL component of the hybrid strategy.**

---

## 5. Adaptive Blanking (Soft Genlock)

### 5.1 Concept

Keep the pixel clock **fixed**. Vary `v_total` (total lines per frame) to match
the measured input frame rate. Extra or fewer **vertical blanking lines** adjust
the output frame period without changing the pixel clock or horizontal timing.

This is what OSSC and RetroTINK effectively do for non-standard input rates.

### 5.2 The Math

Fixed pixel_clock = 25.2 MHz, h_total = 800:
- Line period = 800 / 25,200,000 = **31.746 µs** per line
- Frame period at v_total=525 → 525 × 31.746 µs = **16.667 ms** (60.00 Hz)
- Frame period at v_total=532 → 532 × 31.746 µs = **16.889 ms** (59.21 Hz)
- Frame period at v_total=533 → 533 × 31.746 µs = **16.920 ms** (59.10 Hz)

For exactly 59.18 Hz: v_total = 31,500 / 59.18 = **532.27 lines**

Since we can't do fractional lines, alternate between 532 and 533:
- ~73% of frames at 532 lines (59.21 Hz)
- ~27% of frames at 533 lines (59.10 Hz)
- Weighted average: **59.18 Hz**

With PLL set to 59.184 Hz (FBDIV=145), the residual error is only 0.004 Hz —
adaptive blanking barely needs to intervene (< 1 line adjustment per ~15 frames).

### 5.3 Phase Measurement

**Core 0** (video_capture.c) — timestamps each input VSYNC:
```c
// In sync_irq_handler(), on VSYNC detection:
volatile uint32_t g_input_vsync_time;  // shared with Core 1

static void sync_irq_handler(void) {
    // ... existing VSYNC detection logic ...
    if (vsync_detected) {
        g_input_vsync_time = timer_hw->timerawl;  // µs timestamp
        // ... existing sem_release, etc ...
    }
}
```

**Core 1** (video_output_rt.c) — timestamps each output VSYNC:
```c
// In dma_irq_handler(), when v_scanline wraps to 0:
static uint32_t output_vsync_time;

if (v_scanline == 0) {
    output_vsync_time = timer_hw->timerawl;
}
```

**Phase error** = `output_vsync_time - g_input_vsync_time`
- Positive → output is ahead (leading), need to **slow down** (add blanking)
- Negative → output is behind (lagging), need to **speed up** (remove blanking)

### 5.4 Adjustment Loop

At each output VSYNC (v_scanline wraps to 0):

```c
#define LINE_PERIOD_US  32  // ~31.746 µs per line, rounded

int32_t phase_err = (int32_t)(output_vsync_time - g_input_vsync_time);

// Dead zone: if within ±1 line, don't adjust (prevents oscillation)
if (phase_err > LINE_PERIOD_US) {
    // Output is ahead — slow down by adding blanking lines
    int lines_to_add = phase_err / LINE_PERIOD_US;
    if (lines_to_add > MAX_ADJUST) lines_to_add = MAX_ADJUST;
    rt_v_total_lines = base_v_total + lines_to_add;
} else if (phase_err < -LINE_PERIOD_US) {
    // Output is behind — speed up by removing blanking lines
    int lines_to_remove = (-phase_err) / LINE_PERIOD_US;
    if (lines_to_remove > MAX_ADJUST) lines_to_remove = MAX_ADJUST;
    rt_v_total_lines = base_v_total - lines_to_remove;
    // Clamp: never go below minimum blanking
    if (rt_v_total_lines < MIN_V_TOTAL) rt_v_total_lines = MIN_V_TOTAL;
} else {
    rt_v_total_lines = base_v_total;  // locked — no adjustment
}
```

Where:
- `base_v_total` = nominal v_total for current PLL setting (525 at 60 Hz, ~525 at
  59.184 Hz)
- `MAX_ADJUST` = 4 lines (limits per-frame change to prevent visible glitches)
- `MIN_V_TOTAL` = v_active + v_sync + minimum blanking (e.g., 480+2+10 = 492 for
  480p)

### 5.5 Where v_total Is Consumed

The DMA ISR already uses `rt_v_total_lines` dynamically:

```c
// video_output_rt.c line 474
v_scanline = (v_scanline + 1) % rt_v_total_lines;
```

The extra/fewer lines are **blanking lines** — same command list as v_front_porch
(blanking with no sync). No command list rebuild needed. HSTX simply outputs more
or fewer blanking lines before wrapping to the next frame.

### 5.6 Convergence Behaviour

Starting from 60 Hz output, 59.18 Hz input:
1. Output immediately leads input by ~0.82 frames/second
2. Phase error grows by ~231 µs per frame (~7.3 blanking lines worth)
3. After first measurement, adjustment loop adds ~7 extra lines → v_total ≈ 532
4. Phase error converges to within ±1 line over 3–5 frames
5. Steady state: v_total alternates between 532/533, average = 59.18 Hz

With PLL pre-set to 59.184 Hz, convergence is near-instant (residual < 1 line).

---

## 6. Audio Considerations

### 6.1 ACR (Audio Clock Recovery)

ACR N/CTS values are based on **pixel clock**, which changes only when the PLL
switches — not with adaptive blanking. When genlocked:

| Parameter | 60 Hz (25.2 MHz) | 59.18 Hz (24.857 MHz) |
|-----------|-------------------|------------------------|
| N (48kHz) | 6144 | Non-standard — use HW CTS |
| CTS | 25200 | Derive from pixel clock |

For non-standard pixel clocks, set N to the standard value (6144 for 48kHz) and
let **hardware CTS measurement** derive the correct CTS. Most HDMI sinks support
this. Alternatively, compute CTS manually:

```
CTS = pixel_clock × N / (128 × sample_rate)
CTS = 24,857,143 × 6144 / (128 × 48000) = 24,857 (approx)
```

### 6.2 Samples Per Line

The existing `samples_per_line` calculation already derives from actual pixel
clock and h_total (`video_output_rt.c:524-532`). Since pixel clock changes with
PLL but h_total stays at 800, this auto-adjusts. No change needed.

### 6.3 Variable v_total and Audio FIFO

With adaptive blanking, v_total varies by ±1–2 lines per frame. This means the
number of audio samples per frame varies slightly (~1.5 samples per line at
48kHz). The existing audio buffer fill-level feedback loop (ASRC) already handles
rate mismatch — it was designed for the 55.5kHz → 48kHz conversion. The additional
±1 line variance is negligible (~0.03% rate change per adjustment).

---

## 7. Hybrid Architecture (Recommended)

### 7.1 Overview

```
┌─────────────────────────────────┐
│         PHASE 1 (Boot)          │
│  Measure input rate (~60 frames)│
│  Select closest PLL config      │
│  Switch PLL_USB → clk_hstx      │
│  Gets within ~0.4 Hz of target  │
└──────────────┬──────────────────┘
               │
┌───────────────▼─────────────────┐
│     PHASE 2 (Continuous)        │
│                                 │
│  Core 0          Core 1         │
│  ┌──────────┐   ┌────────────┐  │
│  │ VSYNC IRQ│──→│ DMA ISR    │  │
│  │ timestamp │  │ phase err  │  │
│  │ (shared)  │  │ adjust     │  │
│  └──────────┘   │ v_total    │  │
│                 │ ±1-2 lines │  │
│                 └────────────┘  │
│  Output locks to input          │
│  within 3-5 frames              │
└─────────────────────────────────┘
```

### 7.2 Phase 1: Static PLL Selection at Boot

1. Boot with default PLL_SYS at 126 MHz (60 Hz output).
2. Measure input frame rate over ~60 frames using CSYNC timestamps.
3. If input ≈ 59.18 Hz:
   - Reprogram **PLL_USB** to 124.286 MHz (REFDIV=2, FBDIV=145, PD1=7, PD2=1).
   - Switch clk_hstx AUXSRC to `CLKSRC_PLL_USB`.
   - PLL_SYS stays at 126 MHz — **Core 0 capture timing unaffected**.
4. If input ≈ 60 Hz: keep current config, no genlock needed.

### 7.3 Phase 2: Adaptive Blanking for Residual

5. Core 0 timestamps each input VSYNC (shared volatile).
6. Core 1 computes phase error at each output VSYNC.
7. Adjusts `rt_v_total_lines` by ±1–2 lines per frame.
8. Output phase-locks to input within 3–5 frames.

### 7.4 Phase 3: Lock Detection and OSD

9. When phase error stays within ±1 line for N consecutive frames → "LOCKED".
10. Show genlock status on OSD: `GENLOCK: LOCKED` / `GENLOCK: TRACKING` / `OFF`.

---

## 8. PLL Configuration Table

Pre-computed PLL settings for common target rates. All use REFDIV=2, CSR_CLKDIV=5,
h_total=800, v_total=525 (standard 480p blanking before adaptive adjustment).

| Target Hz | FBDIV | PD1 | PD2 | VCO (MHz) | sys_clk (MHz) | pixel (MHz) | Actual Hz | Error |
|-----------|-------|-----|-----|-----------|---------------|-------------|-----------|-------|
| 58.78 | 144 | 7 | 1 | 864 | 123.429 | 24.686 | 58.776 | −0.01% |
| **59.18** | **145** | **7** | **1** | **870** | **124.286** | **24.857** | **59.184** | **+0.007%** |
| 59.59 | 146 | 7 | 1 | 876 | 125.143 | 25.029 | 59.593 | — |
| **60.00** | **147** | **7** | **1** | **882** | **126.000** | **25.200** | **60.000** | **exact** |

Note: FBDIV=147 with REFDIV=2 gives VCO=882 MHz and sys_clk=126 MHz — identical
to the current REFDIV=1, FBDIV=105, PD1=5, PD2=2 configuration. Either works for
60 Hz; the REFDIV=2 variant just uses a different PLL path.

For 240p mode (h_total=1600, v_total=262):

| Target Hz | FBDIV | PD1 | PD2 | pixel (MHz) | Actual Hz |
|-----------|-------|-----|-----|-------------|-----------|
| 59.18 | 145 | 7 | 1 | 24.857 | 59.295 |
| 60.00 | 147 | 7 | 1 | 25.200 | 60.114 |

240p rates differ from 480p due to different h_total × v_total. Adaptive blanking
compensates for the remaining error in both modes.

---

## 9. Implementation Plan

### 9.1 User Interface

- OSD menu item: **"Genlock: On"** / **"Genlock: Off"**.
- When locked, show **"Genlock: Locked (59.18 Hz)"**.
- Toggled via BACK button or dedicated key.
- Persist in NVM if available.

### 9.2 Boot Sequence

```
main()
  ├── set_sys_clock_khz(126000)     // PLL_SYS = 126 MHz (fixed)
  ├── stdio_init_all()
  ├── video_capture_init()
  ├── video_output_init()           // clk_hstx from CLK_SYS initially
  ├── multicore_launch_core1()
  │
  ├── // Wait for first 60 frames, measure input rate
  ├── if (genlock_enabled && measured_rate < 59.5) {
  │       genlock_switch_pll_usb(870000000, 7, 1);  // 124.286 MHz
  │       genlock_switch_hstx_to_pll_usb();
  │       video_output_request_resync();
  │   }
  │
  └── video_capture_run()           // Main loop with adaptive blanking
```

### 9.3 PLL_USB Switch (Core 0)

```c
static void genlock_switch_pll_usb(uint32_t vco_freq, uint pd1, uint pd2) {
    // 1. Reprogram PLL_USB (currently unused by NeoPico-HD)
    pll_init(pll_usb, 2, vco_freq, pd1, pd2);  // REFDIV=2

    // 2. Switch clk_hstx source from CLK_SYS to PLL_USB
    //    Must disable clk_hstx briefly to change AUXSRC
    clock_configure_int_divider(
        clk_hstx,
        0,  // No glitchless mux
        CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
        vco_freq / (pd1 * pd2),  // src_freq
        1                         // int_divider
    );

    // 3. Trigger HSTX resync on Core 1
    video_output_request_resync();
}
```

**Important**: This must happen while HSTX DMA is in a safe state. The mode-switch
mechanism already handles this — use the same `pending_mode` / `apply_mode` path
or a new `pending_clock_change` flag.

### 9.4 Phase Measurement (Core 0 → Core 1)

```c
// Shared state (video_capture.c)
volatile uint32_t g_input_vsync_time_us;

// In sync_irq_handler(), on VSYNC detection:
g_input_vsync_time_us = timer_hw->timerawl;

// Core 1 reads g_input_vsync_time_us at its own VSYNC
// (timer_hw->timerawl is accessible from both cores, no sync needed)
```

### 9.5 Adaptive v_total Adjustment (Core 1)

In `video_output_handle_vsync()` or at the v_scanline=0 point in the DMA ISR:

```c
if (genlock_active) {
    uint32_t out_time = timer_hw->timerawl;
    uint32_t in_time = g_input_vsync_time_us;
    int32_t phase_err = (int32_t)(out_time - in_time);

    // Wrap-around safe: int32_t handles 32-bit timer rollover
    // (works as long as |phase_err| < 2^31 µs ≈ 35 minutes)

    if (phase_err > 32) {
        // Output ahead — add blanking lines to slow down
        int adj = phase_err / 32;
        if (adj > 4) adj = 4;
        rt_v_total_lines = genlock_base_v_total + adj;
    } else if (phase_err < -32) {
        // Output behind — remove blanking lines to speed up
        int adj = (-phase_err) / 32;
        if (adj > 4) adj = 4;
        rt_v_total_lines = genlock_base_v_total - adj;
        if (rt_v_total_lines < GENLOCK_MIN_V_TOTAL)
            rt_v_total_lines = GENLOCK_MIN_V_TOTAL;
    } else {
        rt_v_total_lines = genlock_base_v_total;
    }
}
```

### 9.6 Audio / ACR Update

When switching to genlock pixel clock (24.857 MHz):

```c
// Non-standard pixel clock — use N=6144 and compute CTS:
// CTS = pixel_clock_hz * N / (128 * sample_rate)
// CTS = 24857143 * 6144 / (128 * 48000) = 24857 (approx)
//
// Or: let hardware CTS auto-measurement handle it (preferred).
```

The existing `configure_audio_packets()` already derives `samples_per_line` from
actual pixel clock (`video_output_rt.c:524-532`). After PLL switch, calling
`configure_audio_packets(48000)` with the new clock will auto-adjust.

### 9.7 HSTX Clock After PLL Switch

When using PLL_USB for clk_hstx:
- PLL_SYS (126 MHz) continues to drive clk_sys for Core 0 capture, audio PIO, etc.
- clk_hstx derives from PLL_USB at the genlock frequency.
- **CSR_CLKDIV stays at 5** — the PLL output is already at the right frequency.
- No impact on `interp0` LUT timing or DMA bandwidth (those use clk_sys).

---

## 10. Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| TV rejects ~59.18 Hz | No picture | User toggle: default OFF (60 Hz). OSD shows state. |
| PLL_USB reprogram glitches HSTX | Brief garbage on screen | Do PLL switch during VSYNC blanking window (1.4 ms available, PLL locks in ~100–500 µs). Disconnect GPIO beforehand (same as `hstx_resync` pattern). |
| Adaptive blanking visible as jitter | Subtle vertical shift | Limit adjustment to ±2 lines/frame. With PLL pre-set, corrections are rare (< 1 line per 15 frames). |
| Phase measurement jitter | Oscillation around lock | Dead zone of ±1 line (~32 µs). Low-pass filter the phase error if needed. |
| sys_clk change breaks capture | PIO timing drift on Core 0 | **Avoided**: PLL_USB approach keeps PLL_SYS/clk_sys at 126 MHz. Core 0 unaffected. |
| Audio glitch on PLL switch | Click/pop | Mute audio during switch, unmute after resync. ~50 ms silent gap. |
| Timer rollover | Phase error wraps | int32_t comparison handles 32-bit rollover naturally (max |error| < 35 min). |

---

## 11. Implementation Checklist

| # | Item | Where | Notes |
|---|------|-------|-------|
| 1 | `genlock_enabled` config flag | video_capture.c | Already stubbed (line 380) |
| 2 | VSYNC timestamp in sync IRQ | video_capture.c | Add `timer_hw->timerawl` read |
| 3 | PLL_USB init for genlock freq | main.c or video_capture.c | `pll_init(pll_usb, 2, 870MHz, 7, 1)` |
| 4 | clk_hstx AUXSRC switch | video_output_rt.c | New helper: `video_output_set_hstx_clock()` |
| 5 | Adaptive v_total in DMA ISR | video_output_rt.c | Modify VSYNC handler |
| 6 | ACR update for new pixel clock | video_output_rt.c | Re-call `configure_audio_packets()` |
| 7 | Lock detection state machine | video_output_rt.c | Track consecutive locked frames |
| 8 | OSD genlock status display | osd.c | "Genlock: Locked/Tracking/Off" |
| 9 | Toggle via button | video_capture.c | BACK button handler |
| 10 | NVM persistence | config.c (future) | Optional |

---

## 12. Existing Code Hooks

Code that already exists and supports genlock:

| What | Where | Status |
|------|-------|--------|
| `genlock_enabled` flag | `video_capture.c:380` | Declared, unused |
| `SYS_CLK_GENLOCK_KHZ` = 124100 | `video_capture.c:373` | Defined, unused |
| `SYS_CLK_60HZ_KHZ` = 126000 | `video_capture.c:372` | Defined, unused |
| `rt_v_total_lines` dynamic modulo | `video_output_rt.c:474` | Already dynamic |
| `hstx_resync()` | `video_output_rt.c:212` | Working, GPIO-safe |
| `video_output_request_resync()` | Core 0 → Core 1 flag | Working |
| `configure_audio_packets()` | `video_output_rt.c:520` | Derives from actual pixel clock |
| `g_input_vsync_time` | — | **Needs to be added** |

---

## 13. References

- **RP2350 Datasheet §8.6** — PLL structure, FBDIV, POSTDIV, VCO range, lock
  behaviour.
- **RP2350 Datasheet §8.1** — Clock generators, clk_hstx AUXSRC, GPIN, frequency
  counter.
- **Pico SDK `pll.c`** — `pll_init()`, `set_sys_clock_pll()`, PLL programming
  sequence.
- **HSTX_IMPLEMENTATION.md** — HSTX clock chain, command lists, 800 cycles/line.
- **cps2_digiav** — Si5351-based framelock, V_STARTLINE, ASRC audio.
- **OSSC** — TVP7002 HPLL for horizontal sync, static PLL presets (no adaptive
  tracking).
