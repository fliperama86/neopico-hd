# Known Issues

This document tracks known hardware and software limitations of the NeoPico-HD project.

## HDMI Sync Stability

### 1. Soft Reset Reliability

**Issue**: Performing a soft reset on the RP2350 (e.g., via `picotool` or a reset button) often results in a "No Signal" state on the connected TV or scaler.
**Symptoms**: The HDMI sink fails to handshake with the HSTX hardware after a reboot.
**Workaround**:

1. Power OFF the Neo Geo MVS.
2. Reset the NeoPico-HD (Pico).
3. Power ON the Neo Geo MVS.

### 2. HDMI 5V Power Requirement

**Issue**: Lack of +5V on the HDMI connector's power pin.
**Symptoms**: "No Signal" on specific scalers, most notably the **RetroTINK** series, TVs and some monitors.
**Observations**:

- The **Morph4K** is more forgiving and can often sync without the 5V line.
- The **RetroTINK** (and many standard TVs) will fail to detect any signal if the 5V rail is not present on the HDMI cable.
  **Requirement**: You **must** wire +5V to the HDMI connector's power pin (Pin 18) for reliable operation.

### 3. Power-Domain Back-Feed During Startup

**Current hardware note**: NeoPico-HD PCB revisions in active use already route MVS digital lines through proper level shifting. PCLK/BCK conditioning via Schmitt-trigger stage is also present.

**Observation**: In some setups, the Pico can remain partially powered when `VSYS` is removed while other rails/signals are still connected.

**Likely cause**: Back-feed through mixed power domains (USB/HDMI 5V, level-shifter paths, or I/O clamp structures depending on the exact wiring and component choices).

**Risk**:

- Non-deterministic rail ramp/decay behavior.
- Startup transients that can disturb digital capture lock.
- Intermittent bring-up failures that disappear after a later relock/reset.

**Recommended hardware mitigations**:

1. Use level shifters/translators with **partial-power-down tolerance / Ioff** support.
2. Gate translator **OE** so MVS -> Pico signals are Hi-Z until 3.3V is stable.
3. Prevent source back-feeding between power inputs (USB vs external 5V) using proper power-path design (ideal diode/power mux).
4. Keep local decoupling close to VSYS/3V3 and translator rails.

### 4. Intermittent Scratched Audio on Cold Power-Up

**Issue**: When MVS and Pico power up together, audio can start heavily scratched/corrupted.

**Behavior**:

- Issue is intermittent (not every boot).
- A later Pico reset or capture relock often clears it immediately.

**Working hypothesis**: I2S capture can occasionally lock with bad startup phase/alignment during power transients, then remain in an active-but-corrupted state until capture is re-armed.

**Software status**:

- Startup mute/warmup is used.
- Capture includes watchdog restart on sustained inactivity.
- Current firmware adds a one-shot post-warmup relock path to emulate the "reset after clocks settle" recovery, without adding ISR load.

---

## OSD & UI

- **Current Status**: OSD is currently disabled in the main branch to prioritize HDMI signal stability during video capture development.
