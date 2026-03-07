# Genlock — Implementation Analysis

## Problem

MVS runs at ~59.18 Hz. HDMI output at 60 Hz causes periodic frame repeats (~every 2s), visible as stutter in scrolling games.

---

## Option 1: Hardware Genlock (XIN Replacement)

Replace the RP2350's 12 MHz crystal with the MVS 12 MHz clock. Since the PLL is hardwired to XOSC, feeding MVS 12 MHz directly into XIN makes the entire system clock tree phase-locked to the Neo Geo — true hardware genlock with **zero software changes** to clock configuration.

### Why 12 MHz

The MVS master clock is 24 MHz; a 12 MHz derived clock is available on the board. This matches the RP2350's crystal frequency exactly, so all existing PLL divider settings (FBDIV, POSTDIV1, POSTDIV2) stay identical.

### PCB Changes

1. **Do not populate** the 12 MHz crystal
2. **Route MVS 12 MHz** to the **XIN pin** with signal conditioning:

```
MVS 12MHz ──[100R]──┬── XIN
                     │
                   [47pF]
                     │
                    GND
```

3. **XOUT**: leave floating or tie through 1M to XIN

### XOSC Configuration

Configure XOSC in bypass/external clock mode:

```c
xosc_hw->ctrl = XOSC_CTRL_FREQ_RANGE_VALUE_1_15MHZ;
hw_set_bits(&xosc_hw->ctrl, XOSC_CTRL_ENABLE_VALUE_ENABLE << XOSC_CTRL_ENABLE_LSB);
```

### What This Gets You

- **PLL_SYS config unchanged**: 12 MHz × FBDIV / POSTDIV = 126 MHz (same as today)
- **clk_sys, clk_hstx, PIO clocks** — everything phase-locked to MVS
- **Zero drift**: no frame repeats, no stutter in scrollers
- **No software genlock logic needed**
- **Audio coherence**: pixel clock and audio timing are synchronous from the first frame

### Concerns

1. **No MVS = No Boot**: Accept it, use ROSC fallback, or add a CMOS mux (SN74LVC1G3157) to switch between crystal and MVS 12 MHz.
2. **Startup Sequencing**: XOSC has a built-in stabilization counter. ROSC can serve as initial clock if there's a race.
3. **Signal Quality**: XIN accepts CMOS square waves. 100R + 47pF filters ringing. PLL VCO provides jitter filtering.
4. **Cold Boot Audio**: May improve — HDMI output is synchronous from the first frame.

---

## Option 2: Dynamic VTOTAL Modulation (Software)

The professional scan converter approach. No hardware changes needed. Measures the phase relationship between MVS and HDMI VSYNCs in real-time and modulates `v_total_lines` by ±1 to track.

### How It Works

1. On each **MVS VSYNC** (Core 0, `video_capture_run` loop), snapshot a free-running timer
2. On each **HDMI VSYNC** (Core 1, `vsync_callback`), snapshot the same timer
3. Compute phase difference: `hdmi_vsync_time - mvs_vsync_time`
4. If HDMI is ahead (phase drifting positive) → set `rt_v_total_lines = 526` for one frame (slow down)
5. If HDMI is behind (phase drifting negative) → set `rt_v_total_lines = 524` for one frame (speed up)
6. Otherwise → keep nominal `rt_v_total_lines = 525`

### Why It Works

The DMA ISR already reads `rt_v_total_lines` directly every scanline:

```c
v_scanline = (v_scanline + 1) % rt_v_total_lines;
```

Writing `rt_v_total_lines` is a single `uint16_t` store — atomic on ARM. No need to disable IRQs, rebuild command lists, or resync HSTX. The ISR picks up the new value on the next wrap.

### Implementation Sketch

```c
// Shared between cores (written by Core 0, read by Core 1)
static volatile uint32_t g_mvs_vsync_timestamp;

// Core 0 — video_capture_run(), on each MVS VSYNC:
g_mvs_vsync_timestamp = timer_hw->timerawl;

// Core 1 — vsync_callback(), on each HDMI VSYNC:
uint32_t mvs_ts = g_mvs_vsync_timestamp;
uint32_t hdmi_ts = timer_hw->timerawl;
int32_t phase_us = (int32_t)(hdmi_ts - mvs_ts);

// Dead zone prevents oscillation
#define GENLOCK_THRESHOLD_US 200
if (phase_us > GENLOCK_THRESHOLD_US) {
    rt_v_total_lines = 526;  // stretch — slow down HDMI
} else if (phase_us < -GENLOCK_THRESHOLD_US) {
    rt_v_total_lines = 524;  // shrink — speed up HDMI
} else {
    rt_v_total_lines = 525;  // nominal
}
```

### Advantages Over Static Genlock

| | Static Genlock | Dynamic VTOTAL |
|---|---|---|
| Compile-time config | Required | Not needed |
| Tracks real MVS clock | No (assumes fixed fps) | Yes (measured per-frame) |
| Handles clock drift | No | Yes |
| Pixel clock | Non-standard | Standard 25.2 MHz |
| ACR packets | Need custom CTS | Standard CTS |
| TV compatibility | Most TVs accept it | Better (standard pixel clock) |
| Visible artifact | Stutter if estimate is off | Invisible (1-line = 0.19%) |

### Design Considerations

- **Hysteresis**: The ±200µs dead zone prevents hunting. Tune on hardware — too tight causes oscillation, too wide allows visible drift before correction.
- **Max modulation**: ±1 line per frame only. Larger jumps risk visible glitch.
- **Audio packet scheduling**: `hstx_di_queue` uses `cached_v_total_lines` for audio scheduling. Keep this at nominal (525); ±1 line modulation is negligible for audio timing.
- **Phase wraparound**: Signed subtraction of uint32 handles timer wraparound naturally.
- **240p mode**: Same principle applies but with `rt_v_total_lines` based on 262 (±1 → 261 or 263).
- **Enable/disable**: Should be feature-flagged and toggleable at runtime (e.g., OSD menu option).

### Existing Infrastructure

The codebase already has everything needed:

- `video_frame_count` — HDMI frame counter (Core 1)
- `g_frame_count` — MVS frame counter (Core 0)
- `vsync_callback` — called on every HDMI VSYNC (Core 1)
- `rt_v_total_lines` — directly used in DMA ISR, writable at any time
- `timer_hw->timerawl` — free-running µs timer, accessible from both cores

---

## Option 3: Static Sysclk Match (Current/Legacy)

`NEOPICO_EXP_GENLOCK_STATIC=ON` + `NEOPICO_GENLOCK_TARGET_FPS_X100=5918`

Adjusts sysclk at boot to approximate MVS frame rate. Simple, mostly works, but:
- Not phase-locked — will still drift if MVS clock differs from assumed value
- Non-standard pixel clock requires custom ACR CTS
- Compile-time only — can't adapt to different boards

### Alternative: VTOTAL Match (Static)

`NEOPICO_EXP_VTOTAL_MATCH=ON` + `NEOPICO_EXP_VTOTAL_LINES=532`

Keeps pixel clock standard but inflates back porch. Same limitations as static sysclk (no feedback).

---

## RP2350 Clock System Quick Reference

| Feature | Status | Details |
|---------|--------|---------|
| External clock on XIN | Yes | CMOS-level, bypass mode |
| PLL FBDIV dynamic adjust | Yes | While locked, may overshoot/undershoot |
| PLL POSTDIV dynamic adjust | No | Requires unlock |
| GPIN clock input | Yes | GP12,14,20,22 — 50 MHz max, divide only |
| GPIN → PLL reference | No | PLL hardwired to XOSC/XIN |
| HSTX fractional divider | No | Integer only |
| Frequency counter (FC0) | Yes | Built-in, configurable accuracy |

## Recommendation

1. **Now**: Implement dynamic VTOTAL modulation — no hardware changes, superior to static genlock, uses existing infrastructure.
2. **Next PCB rev**: Add MVS 12 MHz → XIN option for true hardware genlock.
