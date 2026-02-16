# NeoPico-HD Scratch Book

Working notes for intermittent audio corruption, power-domain behavior, and fast validation experiments.

## Session Memory Policy

- This file is the persistent memory log across sessions.
- Update on nearly every interaction with concise, factual notes.
- Track:
    - User preferences (process/style constraints).
    - What worked, what failed, and regressions observed.
    - Active hypotheses and safest next steps.
- Keep append-only, timestamped entries.

## User Preferences (Persistent)

- Do not run commands or change code without informing first.
- Proceed methodically, one step at a time, validating before continuing.
- Do not commit unless explicitly requested.
- Prefer factual/source-grounded decisions; avoid guesses.
- Prefer introducing non-trivial/experimental changes behind compile-time feature flags (`#ifdef`/macros), off by default.
- For experiments, prioritize reversible rollout and minimal-regression paths over invasive direct changes.

## Current Session Highlights

### 2026-02-14

- Goal: Port selftest OSD layout to `fast_osd` while preserving sync stability.
- Implemented: `fast_osd` geometry/color/special glyph support and selftest-style layout module.
- Confirmed working: static layout + basic updates through `fast_osd`.
- Regressions observed:
    - Multiple attempts at diagnostics sampling (PIO/DMA sampler and capture-pause strategies) caused eventual sync drops or no-signal behavior.
    - Disabling `g_sm_pixel` in runtime control flow (`pio_sm_set_enabled(..., false)`) was identified by user as destabilizing.
    - Even presence of extra timing-sensitive code in capture path can perturb long-run stability.
- Current safe stance:
    - Keep capture hot path minimal.
    - Avoid runtime stop/start of pixel SM outside explicit hardware reset paths.
    - Prefer incremental, easily reversible experiments with strict A/B validation.
- User-directed stabilization change:
    - Removed diagnostics and OSD handling code from `video_capture.c` capture path entirely.
    - Rationale: even dormant/non-visible diagnostics code presence perturbs long-run sync stability.
    - Operational preference: keep capture loop strictly focused on VSYNC + DMA + pixel conversion.
- Code review finding (video pipeline optimizations):
    - Found high-risk logic hole in `video_pipeline_scanline_callback`: `osd_src` can be dereferenced when OSD line is not active and `src==NULL` (unsigned underflow index), and `src==NULL` path currently can leave `dst` unwritten.
    - Action guidance: restore explicit `src==NULL` fill path and only access `osd_framebuffer[...]` when `osd_line_active` is true.
    - Applied minimal safety patch in `video_pipeline.c`:
        - Early return with deterministic `dst` fill when `src==NULL`.
        - Guarded OSD framebuffer read behind `osd_line_active` check.
        - Build/lint clean after patch.

## Context

- Symptom: On some cold power-ups (MVS + Pico on together), HDMI audio starts heavily scratched/corrupted.
- Recovery observed: Pressing Pico reset often fixes it immediately.
- Additional observation: Pico can remain partially powered in some "off" sequences when HDMI is still connected.

## Current Working Model

1. I2S capture can occasionally lock on a bad startup phase/alignment during rail transients.
2. This can produce an active-but-corrupted audio stream (not a silent timeout case).
3. Hardware/power-domain backfeed can leave the board in a "zombie" voltage state, increasing nondeterministic startup behavior.
4. A relock/rearm later (reset or forced restart of capture) restores clean audio.

## Code Changes Already Applied

- Added one-shot post-warmup capture relock in `src/audio/audio_subsystem.c`.
- Behavior:
    - Keep muted at startup.
    - After warmup, stop/start audio capture once (hardware-style rearm).
    - Keep muted briefly, then unmute with flushed state.
- Build status: passes (`./scripts/build.sh`).

## Hardware Facts Reported in This Session

- USB disconnected during tests.
- DDC (SCL/SDA) unconnected/floating.
- HDMI pin 18 (+5V) connected to VSYS (later diode added in series for tests).
- HPD pull-up to 5V was removed for testing.
- CEC temporarily disconnected for testing.
- With some configurations, VSYS still sits around ~2.5V with HDMI connected.
- Adding VSYS bleed (2.2k then 1k to GND) did not fully collapse the ghost voltage.
- Pressing Pico reset can make ghost voltage disappear.

## SpotPear Daughterboard Comparison (No Ghost-Voltage Case)

Observed from provided SpotPear schematic:

- HDMI TMDS lanes include **series resistors (270R) on each P/N conductor**.
- DDC/CEC side includes pull resistor network (2k2 shown on the daughterboard), which differs from current NeoPico-HD behavior where DDC is left floating and CEC is tied to MCU pin with local pull.
- HPD handling differs from current NeoPico-HD setup (which has an onboard pull-up footprint `R2` from local `+5V` to HDMI HPD net).

Contrast with current NeoPico-HD PCB (`hardware/neopico-hd/neopico-hd.kicad_pcb`):

- TMDS nets (`D0+/D0-/D1+/D1-/D2+/D2-/CLK+/CLK-`) are routed directly between RP2350 GPIO and HDMI connector pins (no per-lane series resistor footprints).
- HDMI HPD net is tied via `R2` 10k to local `+5V`.
- CEC (`J1 pin 13`) is directly tied to `GPIO8` and pulled to `+3V3` by `R1` 27k.

Implication:

- Direct TMDS wiring is a strong candidate for ghost-rail injection through sink-side TMDS bias/termination when local power is absent.
- SpotPear series impedance likely reduces that DC injection path enough that rails do not latch in a zombie state.

## Most Likely Low-Level Injection Path

1. HDMI sink is still powered and applies bias/common-mode network on TMDS receiver side.
2. With direct TMDS-to-GPIO routing, current enters RP2350 pad/clamp structures.
3. Leakage backfeeds `3V3` and then `VSYS` through onboard power paths, parking rails around ~2.5V.
4. Brown/zombie rail causes non-deterministic peripheral startup; I2S lock can come up mis-phased.
5. A reset reinitializes pad/peripheral state and capture lock, clearing both ghost state and scratched audio.

## Fast Bodge Order (No PCB Respins)

1. Add temporary **series resistance on each TMDS line** (all 8 traces): start at 220R, then test 150R if link margin drops.
2. Keep HPD pull-up removed (or isolate HPD from local +5V) during debug.
3. Keep CEC disconnected for now (optional feature path, not required for video/audio bring-up).
4. Keep firmware one-shot audio relock enabled to mask residual startup race while hardware bodges are tested.

Success criteria:

- "Off but HDMI connected" rail test: `3V3` and `VSYS` should fall near 0V instead of parking near ~2.5V.
- Cold-boot audio scratch rate should collapse to near zero without needing manual reset.

## Inferences to Verify

- If VSYS bleed is ineffective, injection likely enters 3V3/IO domain first, then back-propagates.
- Reset affecting ghost state suggests MCU pad/peripheral state contributes to the leakage path.

## Test Matrix

### A. Rail Decay / Backfeed

1. Measure `3V3` and `VSYS` with:
    - USB disconnected
    - MVS off/disconnected
    - HDMI connected
2. Repeat while holding Pico reset low.
3. Add `3V3 -> GND` bleed:
    - Try 1k, then 470R if needed.
4. Compare with HDMI physically disconnected.

Pass target:

- In "off" state, both rails collapse close to 0V (ideally <0.3V).

### B. Audio Startup Reliability

1. Cold boot loop with current firmware relock enabled.
2. Record fail rate across N boots (e.g., 30).
3. Compare with:
    - HDMI connected before power
    - HDMI connected after power

## Near-Term Practical Mitigations

- Keep startup relock firmware path enabled.
- Avoid entering brown/zombie states between tests.
- Prefer true cold-off between iterations (discharge rails).

## Fact-Checked External Guidance (Design Pattern)

1. In MCU-driven DVI/HDMI-style outputs (PicoDVI ecosystem), **per-line TMDS series resistors are standard practice** and values around 220R are common.
2. HPD is a sink-driven signal; source-side forcing high is not recommended.
3. DDC should be treated as real I2C (not left floating) in standards-aligned HDMI interfaces.
4. Back-drive/reverse-current protection is a known requirement in practical HDMI source hardware.

Working implication for this board:

- Your observed 2.5V zombie rail and reset-sensitive behavior are consistent with missing/insufficient injection limiting on direct TMDS paths in an off-powered source state.

## Next PCB Revision Checklist

1. Explicit power-path isolation for HDMI-related backfeed scenarios.
2. Verify all interface parts have partial-power-down/Ioff behavior.
3. Hold logic inactive until rails are valid (RUN supervisor and/or OE gating strategy).
4. Preserve TMDS signal integrity while adding isolation (avoid naive pull resistors on TMDS pairs).

## Session Log

### 2026-02-08

- Root-cause analysis performed against docs + source + staged test changes.
- Strong hypothesis formed: startup I2S phase lock issue exacerbated by power-domain behavior.
- Firmware one-shot relock added and built successfully.
- Docs updated:
    - `README.md` hardware integrity guidance
    - `docs/KNOWN_ISSUES.md` power-domain/backfeed and cold-boot audio section
- Ongoing hardware debug focused on HDMI-associated ghost powering.
- External reference cross-check completed:
    - Series TMDS resistors on MCU-output designs are common (220R frequently used in PicoDVI-derived boards).
    - HPD/DDC handling guidance aligns with not forcing HPD high and not leaving DDC floating.

### 2026-02-16

- Focus shifted to `src/video/video_pipeline.c` hot-path optimization.
- Stable/kept changes:
    - Replaced manual fill loops with `memset` for black line clears.
    - Simplified active-window check to unsigned range check (`mvs_line_u32 >= MVS_HEIGHT`).
    - Latched OSD visibility once per VSYNC (`osd_visible_latched`) to avoid volatile reads each scanline.
- Tried and reverted:
    - Added vertical duplicate line cache (render once per `fb_line`, copy on odd line).
    - User observed this "broke the signal"; cache path fully removed and build revalidated.
- User preference reinforced in practice:
    - Favor conservative, low-risk timing changes in scanline ISR paths.
    - Roll back immediately when signal integrity regresses, even if optimization looks valid in theory.
- Current caution:
    - Keep OSD framebuffer row indexing guarded by OSD-active range checks; out-of-range accesses can manifest as video instability/corruption.
