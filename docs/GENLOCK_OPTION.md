# Genlock Option (59 Hz vs 60 Hz Output)

This document describes the frame-rate mismatch between MVS input and HDMI output, why it causes slight stutter, and the design for a **user-toggleable genlock option** so output can either follow the source rate (~59.18 Hz) or stay at standard 60 Hz.

---

## 1. The Problem

- **MVS** runs at approximately **59.18 Hz** (Neo Geo; exact value depends on the board’s crystal).
- **NeoPico-HD output** is fixed at **60 Hz** (system clock 126 MHz → 25.2 MHz pixel clock → standard 640×480 or 1280×240 timing).
- The ~0.82% rate mismatch means we periodically **show the same input frame twice** (one frame held for two display refreshes) about every ~2 seconds.
- In smooth side-scrolling games this appears as a **slight stutter or judder**.

---

## 2. How cps2_digiav Handles It (Reference)

The CPS2_digiav reference project (in `~/Projects/references/cps2_digiav`) avoids this by:

1. **Framelocked output rate**
   The output pixel clock is derived from the **source** via a **Si5351C** programmable clock. The output refresh rate equals the source (e.g. 59.19 Hz for Neo Geo, 59.64 Hz for CPS2). No 60 Hz conversion.

2. **V_STARTLINE**
   On each input frame change, the FPGA **repositions** the output scanline counter to a computed line (`V_STARTLINE`) so input and output stay in phase. The FPGA has full control over output timing.

3. **Result**
   They output **non-standard** rates (~59.2–59.6 Hz) to the TV and rely on the display to accept them. Most modern TVs do. NeoPico-HD cannot do mid-stream repositioning (HSTX has no equivalent to V_STARTLINE), but we can still match the **frequency** by changing the system clock (genlock).

---

## 3. Pragmatic Alternatives for NeoPico-HD

| Option | Effort | Stutter | TV compatibility | When to use |
|--------|--------|---------|------------------|-------------|
| **1. Do nothing** | None | Slight | Best (60 Hz) | Current behaviour acceptable |
| **2. Genlock (59 Hz)** | Medium | None | Depends on TV | Prefer smooth motion, accept non-standard rate |
| **3. Genlock + 60 Hz toggle** | More | None in genlock mode | User choice | **Chosen:** smooth when possible, fallback for picky TVs |
| **4. External clock (e.g. Si5351)** | Hardware | None | Same as (2) | New board revision, want genlock without slowing sysclk |
| **5. Smarter repeat/drop at 60 Hz** | Medium | At best reduced | Best | Only if 60 Hz is mandatory and genlock not possible |

**Chosen approach:** Option 3 — **genlock as a software-toggleable option**. Default can stay 60 Hz for maximum compatibility; users who want smooth motion can enable genlock and accept that some displays may not like ~59.2 Hz.

### 3.1 Can we wire the MVS clock to a Pico clock input?

Yes, but with an important constraint.

- **RP2350 clock inputs:** The chip has **GPIN0** and **GPIN1** (e.g. GPIO 20 and 22 on some packages; GPIO 22 is already used for I2S DAT on NeoPico-HD, so only one may be free). The datasheet allows **clk_ref** and **clk_sys** to be driven from these inputs, up to **50 MHz**. So in principle we could run the system from an external clock and get true hardware genlock.
- **What the MVS gives us:** The only clock we currently tap from the MVS is **PCLK at 6 MHz** (pixel clock). That is well under 50 MHz, so we could feed it into GPIN0.
- **The problem:** We need **~25.2 MHz** for the HDMI pixel clock (or ~24.8 MHz when genlocked). We cannot get 25.2 MHz from 6 MHz by *division* alone. The RP2350’s **PLL reference is tied to the crystal (XOSC)**, not to GPIN, so we cannot multiply the 6 MHz from GPIN inside the chip.
- **Practical options if we want “wire MVS clock in”:**
  1. **External PLL (e.g. Si5351):** Multiply MVS 6 MHz → 126 MHz (or 25.2 MHz), feed that into GPIN0. Then clk_sys runs locked to MVS with no software PLL switching. This is effectively **Option 4** (external clock) in the table above; it needs an extra chip and a board change.
  2. **Tap MVS master crystal:** If the MVS board’s crystal (e.g. 12 MHz) is accessible, we could feed it into the Pico’s **XIN** and use it as the PLL reference. The whole system would then be locked to the MVS master clock. This is invasive (replace or share the Pico’s crystal input, tap the MVS board).
  3. **Keep software genlock (Option 3):** Use the known MVS rate (59.18 Hz), reprogram the PLL to the matching sysclk. No extra hardware; same result for display smoothness.

So: **we can** wire an MVS clock to a Pico clock-in pin, but with the current 6 MHz PCLK we cannot run the system and HDMI from it without an external multiplier or tapping the MVS crystal. Option 3 (software genlock) avoids that hardware change.

---

## 4. Toggleable Genlock: Behaviour

### Genlock Off (default)

- System clock = **126 MHz** (current behaviour).
- HSTX pixel clock = 25.2 MHz → **60 Hz** output.
- Works with all displays; slight periodic stutter as today.

### Genlock On

- Use the **known nominal** MVS frame rate (e.g. **59.18 Hz** for Neo Geo). No measurement needed.
- Set system clock so that HSTX produces that rate (e.g. **~124.1 MHz** for 59.18 Hz with current divider).
- Reconfigure **clk_hstx** from the new sysclk.
- Output is **~59.18 Hz**; no frame repeat/drop, no rate-based stutter.
- **Relies on the TV** accepting a non-standard rate (as cps2_digiav does).

---

## 5. Implementation Plan

### 5.1 User interface

- **Recommended:** OSD menu item, e.g. **“Genlock: On”** / **“Genlock: Off”**, toggled by the same button that cycles other video options (e.g. BACK or a dedicated key).
- Show current state whenever the OSD or video-options section is visible.
- Optional: persist the choice in non-volatile config so it survives reboot.

### 5.2 State and persistence

- One flag: e.g. `genlock_enabled` (bool).
- If the project gains non-volatile config, store this flag so the user’s choice is kept across power cycles.

### 5.3 Target system clock (no measurement needed)

- For **Neo Geo MVS** we already know the nominal frame rate: **~59.18 Hz** (board-to-board variance is small). Use that constant.
- Compute **target system clock**: 60 Hz → 126 MHz ⇒ `sysclk_target = 126 * (59.18 / 60.0)` ≈ **124.1 MHz**.
- Use **`vcocalc.py`** (12 MHz input) to get the nearest achievable system clock to `sysclk_target` and use that value when setting the clock (e.g. at build time or in a small lookup).
- **Optional:** For other sources (e.g. CPS2 at ~59.64 Hz) or if you want to match the exact board, you could measure vsyncs over ~1 s and compute `fps`, then `sysclk_target = 126 * (fps / 60.0)`. Not required for MVS-only.

### 5.4 Applying the toggle

- **Turn genlock ON**
  1. Use nominal MVS rate (e.g. 59.18 Hz) and target sysclk (e.g. ~124.1 MHz); call the SDK to set system clock: **`set_sys_clock_pll()`** with vcocalc-derived parameters is recommended for runtime switching; **`set_sys_clock_khz()`** is an alternative but has known issues when called multiple times.
  3. Reconfigure **clk_peri** from the new sysclk if other peripherals depend on it.
  4. Reconfigure **HSTX clock** from the new sysclk (see below).
  5. Optionally trigger an HSTX resync so the next frame starts clean.
  6. Set `genlock_enabled = true`.

- **Turn genlock OFF**
  1. Set system clock back to **126 MHz**.
  2. Reconfigure clk_peri from the new sysclk if other peripherals depend on it.
  3. Reconfigure HSTX clock.
  4. Trigger resync if needed.
  5. Set `genlock_enabled = false`.

All **sysclk changes and HSTX clock reconfiguration** must run on **Core 0**. Resync can be triggered via a flag that **Core 1** already respects (e.g. same mechanism as 240p/480p mode switch).

### 5.5 HSTX clock after sysclk change

- HSTX clock is derived from sysclk. When sysclk changes, the HSTX clock tree must be **reconfigured** with the **new** sysclk (same divider, new source frequency).
- **Option A (recommended):** In **pico_hdmi**, add e.g. `video_output_reconfigure_clock(void)` that performs the same `clock_configure_int_divider(clk_hstx, ...)` as in `video_output_init()`, using **current** `clock_get_hz(clk_sys)`. The app calls it after every sysclk change.
- **Option B:** In the app, after changing sysclk, call a small helper that reconfigures `clk_hstx` from the current sysclk using the same divider as the library.
- After reconfiguring, trigger an **HSTX resync** so the next frame starts from a known state.

### 5.6 Audio / ACR

- When genlock is on, the pixel clock is lower; **ACR** (and thus 48 kHz audio) must match the new clock.
- When toggling genlock, recompute or update **ACR N/CTS** (or whatever the library uses) for the current pixel clock and update the HDMI packet. If the library already derives ACR from the current clock, re-running its init or a “reconfigure” path may be sufficient; otherwise add an explicit ACR update when genlock is toggled.

### 5.7 OSD text

- When the user opens the OSD (or the video-options section), show:
  - Current output mode (e.g. “Output: 480p” / “240p”).
  - **“Genlock: On”** or **“Genlock: Off”**.
- When the user toggles genlock, update the flag, apply the clock and HSTX changes, then refresh the OSD so the label matches the new state.

---

## 6. Implementation Checklist

| Item | Purpose |
|------|--------|
| Config flag | `genlock_enabled` (and persist in NVM if available). |
| UI | OSD menu or dedicated control: “Genlock: On/Off”; show current state. |
| Target sysclk | Use nominal MVS rate (59.18 Hz) → sysclk ≈ 124.1 MHz; vcocalc for exact PLL params. Optional: measure vsyncs for other sources. |
| Sysclk switch | “Genlock On”: set sysclk to genlock target; “Genlock Off”: set to 126 MHz. |
| HSTX reconfigure | After every sysclk change, reconfigure `clk_hstx` from current sysclk (same divider). |
| clk_peri | After every sysclk change, reconfigure `clk_peri` from current sysclk if other peripherals depend on it. |
| Resync | After HSTX reconfigure, trigger one HSTX resync on Core 1. |
| ACR | Update ACR for new pixel clock when genlock is on so HDMI audio remains correct. |
| Core 0 only | Perform sysclk and HSTX clock reconfiguration on Core 0; only resync on Core 1. |

---

## 7. References

- **STREAMING_PIPELINE_PLAN.md** — Line ring, frame sync, and why 40-line streaming was abandoned; mentions cps2_digiav’s V_STARTLINE.
- **HSTX_IMPLEMENTATION.md** — HSTX clock and pixel clock (25.2 MHz from 126 MHz).
- **cps2_digiav** (`~/Projects/references/cps2_digiav`) — Si5351-based framelock, V_STARTLINE, `scanconverter.v`, `video_modes.c`, `sys_controller.c`.
