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

### 5. Rare Periodic HDMI Audio Dropouts (Resolved 2026-07-12)

**Issue**: A brief audio drop every tens of minutes, on all resolutions, both MVS and AES sources, across all firmware versions. Severity depended on the connected sink; some TVs never showed it.

**Root cause**: The Data Island scheduler paced audio with a floor-truncated 16.16 samples-per-line value, delivering ~0.18 samples/s (480p/240p; ~0.09 at 720p) less than the exactly-48000 Hz rate advertised via ACR on the same clock. The sink's audio buffer drained until it concealed with a short mute. Firmware buffers stayed healthy (the SRC servo locks production to the scheduler), so no firmware counter could see it.

**Resolution**: pico_hdmi `b6422ee` (`PICO_HDMI_EXACT_AUDIO_PACING`, default ON) paces with an exact rational accumulator; delivery now matches the ACR rate exactly in every runtime mode. NeoPico bump: `267ae19`. See `docs/HSTX_IMPLEMENTATION.md`, "Pacing & Clock Accuracy".

**Remaining distinct mechanisms** (not covered by this fix):

- A real video sync loss (>100 ms without vsync) triggers an audio re-arm: ~0.5 s mute coinciding with a visible video hiccup. Intentional recovery behavior.
- A Core 1 background stall longer than the queue cushion (~10 ms) splices silence packets. Build with `-DNEOPICO_DIAG_AUDIO_OSD=ON` to watch the `AU<n>` counter on the selftest screen; it climbing during an audible drop indicates this mechanism.

### 6. 720p Transient Corruption on Direct Low-Latency Sinks (Fixed on Affected Sink)

**Issue**: At 720p only, rare split-second bursts of whole-frame TMDS
corruption (purple/green scanline noise, including over the black pillarbox
bars) on sinks that consume the signal directly without re-clocking. Observed
on a Samsung Q80 in Game Mode; the same TV in Normal mode, gaming monitors,
and all scalers (RetroTINK 4K, Morph4K) are clean.

**Rates**: roughly one event per minute during gameplay, roughly one per five
minutes on a static screen. 480p is unconditionally clean in every setup.

**What is known** (2026-07): buffered/re-clocking sinks stay clean, while the
direct Samsung low-latency path can expose the transient. Displayed-content
changes are neither necessary nor sufficient as a trigger; the surviving
correlation is that only builds rendering the live capture ring have glitched.
The previous runtime timing drove clk_hstx at 372 MHz against a 150 MHz
datasheet rating. Main now uses exact-clock reduced-blanking 720p at 320 MHz
clk_hstx, a 64 MHz pixel clock, and 59.979 Hz. This remains above the rated HSTX
clock and uses non-CTA VIC-0 timing, so broad sink compatibility and the rare
sink compatibility remain under test. The affected Samsung result is recorded
below.

A separate orange-line diagnostic proved that one top-of-frame artifact was a
not-ready capture-ring read rather than TMDS corruption. The default-off
`NEOPICO_EXP_RING_FRAME_GUARD` retained the previous complete frame and passed
an initial no-logging smoke test. Continuous USB counter logging itself
disturbed video, so its event rate is not a clean baseline. A direct-TV test of
the corrected MVS release profile, with exact-clock 720p and the frame
guard enabled, also eliminated the magenta transient on the affected Samsung.
Because both changes were present, this result does not isolate which change
was necessary.

**Historical workarounds**: use 480p for direct TV connections, or run 720p
through a scaler or the TV's standard (non-game) mode.

**Validated fix profile** (2026-07-31): exact-clock 64 MHz runtime 720p plus
`NEOPICO_EXP_RING_FRAME_GUARD=ON`. The previously affected Samsung now runs
cleanly in the direct low-latency path. Keep both changes together unless a
controlled A/B later isolates the required component.

**Tracking**: investigation log, experiment queue, and production mitigation
analysis in [`720P_PURPLE_GLITCH.md`](720P_PURPLE_GLITCH.md) and
[`720P_SAMSUNG_GAME_MODE_INVESTIGATION.md`](720P_SAMSUNG_GAME_MODE_INVESTIGATION.md).

---

## OSD & UI

- **Current Status**: OSD and the root settings menu are enabled by default.
