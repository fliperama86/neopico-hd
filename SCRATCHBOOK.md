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
- Next-step scaffold added (default OFF):
    - Added feature-flagged experiment module `experiments/menu_diag_experiment.*`.
    - `EXP_MENU_DIAG` defaults to `0` in header.
    - Wired no-op `menu_diag_experiment_init()` and background tick hook from `main.c`.
    - No capture-path modifications; behavior intended unchanged while flag is off.
- EXP_MENU_DIAG micro-step enabled:
    - Enabled `EXP_MENU_DIAG=1` in CMake for test build.
    - Implemented menu button polling + debounce in Core1 background task (experiment module only).
    - On menu open: draw static selftest layout; while visible update at ~1 Hz with spinner-only path.
    - No capture-path changes and no live signal sampling in this step.
- Reverted EXP_MENU_DIAG micro-step for A/B noise comparison:
    - Removed `EXP_MENU_DIAG=1` from CMake definitions.
    - Restored `menu_diag_experiment.c` to no-op scaffold placeholders.
    - Goal: compare against last known stable baseline with experiment behavior disabled.
- Hardware finding:
    - Audio/sync issue during A/B was traced to unplugged GND, not software regression.
- Re-introduced EXP_MENU_DIAG micro-step after wiring fix:
    - Re-enabled `EXP_MENU_DIAG=1` in CMake.
    - Restored Core1 background menu handling (button debounce + OSD toggle).
    - Static selftest layout on open + ~1 Hz lightweight updates only (no capture-path changes).
- Build-config improvement:
    - Converted feature flags to CMake cache params for compile-time A/B:
        - `NEOPICO_EXP_MENU_DIAG` -> `EXP_MENU_DIAG` (0/1)
        - `NEOPICO_ENABLE_DARK_SHADOW` -> `ENABLE_DARK_SHADOW` (0/1)
    - Added fallback guard in `video_capture.c` for `ENABLE_DARK_SHADOW` when not externally defined.
- Verification:
    - Reconfigured and built with `NEOPICO_EXP_MENU_DIAG=ON`; build passed.
- Next incremental experiment (flag ON, capture path untouched):
    - Implemented CSYNC-only live status in `menu_diag_experiment.c`.
    - Core1 background accumulates CSYNC high/low while OSD is visible and emits 1 Hz snapshot.
    - `selftest_layout_update()` now receives only `SELFTEST_BIT_CSYNC` in this phase.
- Next incremental experiment extension:
  - Added PCLK live status alongside CSYNC in `menu_diag_experiment.c`.
  - Same 1 Hz snapshot cadence and same Core1 background-only path.
  - Capture path remains untouched.
- Next incremental experiment extension (more ambitious):
  - Added all remaining video signals at once in `menu_diag_experiment.c`:
    - `SHADOW`, `R0..R4`, `G0..G4`, `B0..B4` (plus existing CSYNC/PCLK)
  - Still Core1 background only and 1 Hz snapshot cadence.
  - No capture-path modifications.
- Audio micro-step started:
  - Added `BCK` only to experiment sampler (Core1 background, 1 Hz snapshot).
  - Current snapshot now includes: full video signals + `BCK`.
  - `WS`/`DAT` intentionally deferred to subsequent steps.
- Audio expansion:
  - Added `WS` and `DAT` to experiment sampler.
  - Current snapshot now includes full video + full audio (`BCK`, `WS`, `DAT`).

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
- 2026-02-16 00:37:45 -03: Re-read required docs (`README.md`, HSTX/OSD/MV1C video/audio docs) to refresh hard constraints before further edits.
- Confirmed non-negotiables to preserve:
    - Core split: Core 0 capture/conversion only; Core 1 HSTX + audio + OSD injection.
    - HDMI timing: exact 800 cycles/line at 25.2 MHz; RAW_N_SHIFTS must remain 1.
    - OSD: overlay during Core 1 scanline doubling ISR; avoid per-pixel branching in inner loop.
    - Video capture: PIO1 Bank 1 contiguous window GPIO 27-44; SHADOW applied before expansion and forces DARK=1.
    - Audio: right-justified, WS high=left, BCK rising-edge sample, SRC ~55.5kHz->48kHz with closed-loop drift control; HDMI audio validity bit must be 0.
- 2026-02-16 00:39:18 -03: Updated self-test default icons in `src/osd/selftest_layout.c` from red cross to neutral gray `-` during reset state.
- Result: untested/selftest-initial state no longer appears as failure; active updates still render green check for pass and red cross for fail.
- Verification: `cmake --build build -j4` succeeded.
- 2026-02-16 00:44:34 -03: Verified self-test behavior for user question (real vs mock).
- Findings:
    - `EXP_MENU_DIAG` is ON by default in `src/CMakeLists.txt`, so diagnostics code is active unless explicitly disabled.
    - Self-test samples live GPIO states (`gpio_get`) for MVS video pins and I2S pins in `menu_diag_experiment_tick_background()`.
    - Pass condition is activity/toggle-based over a ~60-frame window (`hi & lo`), not protocol/frame correctness validation.
- Implication:
    - Not mocked, but it is a lightweight liveness detector and can show false negatives on lines that remain static during the sample window.

- 2026-02-16 00:45:20 -03: Clarified EXP_MENU_DIAG wiring: set via CMake option `NEOPICO_EXP_MENU_DIAG` and propagated to compiler define `-DEXP_MENU_DIAG=<0|1>` in target compile definitions.
- Current workspace state verified: `build/CMakeCache.txt` has `NEOPICO_EXP_MENU_DIAG:BOOL=ON`; compile commands include `-DEXP_MENU_DIAG=1`.

- 2026-02-16 00:47:10 -03: Fixed no-signal OSD regression in `src/video/video_pipeline.c` scanline callback.
- Root cause: early returns on out-of-range/no-source path prevented OSD blit from running when MVS/CSYNC was absent.
- Change: split fast path (`!osd_line_active`) from OSD-active path; when OSD is active and `src==NULL`, render OSD box over black line (`memset` + OSD region blit).
- Result: OSD remains visible with no CSYNC / MVS off while preserving normal source-backed overlay behavior.
- Verification: `cmake --build build -j4` succeeded.

- 2026-02-16 01:00:11 -03: Refined no-signal OSD rendering path after user report of left-column cutoff.
- Change in `src/video/video_pipeline.c`: when OSD line is active and `src` is unavailable, use a static black fallback line (`s_black_line`) and run the same before/OSD/after three-blit path as normal source mode.
- Rationale: keeps horizontal addressing identical across signal/no-signal conditions, avoiding branch-specific placement artifacts.
- Verification: `cmake --build build -j4` succeeded.

- 2026-02-16 01:05:30 -03: User reported HDMI sync drop when opening OSD after no-source fallback-line change.
- Action: reverted fallback-line path in `src/video/video_pipeline.c` (removed `s_black_line` read path).
- Current no-source OSD path restored to lightweight ISR-safe behavior: clear line via `memset`, then blit OSD region only.
- Verification: `cmake --build build -j4` succeeded.
- Hypothesis: flash/extra-memory read pressure in ISR from fallback line path was enough to trigger HSTX underrun.

- 2026-02-16 01:10:12 -03: Addressed intermittent glyph corruption (e.g., title letters glitching) as OSD read/write race.
- Implemented front/back OSD buffering in `src/osd/fast_osd.c`: ISR reads `osd_framebuffer` (front), writers render to `osd_backbuffer` and mark dirty rows.
- Added `fast_osd_commit()` (VSYNC-time row copy) and call from `video_pipeline_vsync_callback()` in `src/video/video_pipeline.c`.
- Kept scanline ISR hot path unchanged for normal per-line blending; only no-source OSD fallback remains lightweight memset + region blit.
- Verification: `cmake --build build -j4` succeeded.

- 2026-02-16 01:12:40 -03: Reverted OSD backbuffer + VSYNC commit experiment due to continued sync drops.
- Reverted files: `src/osd/fast_osd.c`, `src/osd/fast_osd.h`, `src/video/video_pipeline.c` (removed `fast_osd_commit` path).
- Current status restored to previous lightweight single-buffer OSD path and no-source branch (`memset` + OSD-region blit).
- Verification: `cmake --build build -j4` succeeded.
- Working hypothesis: any additional per-frame copy/commit work in Core1 timing path is unsafe.

- 2026-02-16 01:14:35 -03: Tuned no-source OSD ISR branch in `src/video/video_pipeline.c` to reduce timing load.
- Change: replaced full-line black `memset` + OSD blit with split clears (left/right only) plus OSD middle blit.
- Reason: avoids double-writing OSD span (was cleared then immediately overwritten), reducing per-scanline work in no-CSYNC mode.
- Verification: `cmake --build build -j4` succeeded.

- 2026-02-16 01:13:10 -03: User confirmed latest no-source OSD timing tweak is stable ("works as before now").
- Stable kept state: no-source OSD uses split black clears (left/right) + OSD middle blit in `video_pipeline_scanline_callback()`.
- Reinforced constraint: prioritize ISR-cycle reductions over architectural OSD buffer synchronization in Core1 timing path.

### 2026-02-16

- 2026-02-16 10:31:02 -03: User requested no-signal fallback color change from black to dark gray.
- Applied minimal video-path-only change in `src/video/video_pipeline.c`:
  - Added `NO_SIGNAL_COLOR_RGB565` = `0x2104` (dark gray).
  - Added `video_pipeline_fill_rgb565(...)` helper to fill packed RGB565 words.
  - Replaced no-signal black `memset` fills with dark-gray fills in both OSD-inactive and OSD-active/no-source paths.
- Scope intentionally small: no capture-path logic changes, no audio-path changes, no flag behavior changes.
- 2026-02-16 10:32:31 -03: User requested brighter no-signal fallback; changed `NO_SIGNAL_COLOR_RGB565` from `0x2104` (dark gray) to `0x7BEF` (mid gray) in `src/video/video_pipeline.c`.
- 2026-02-16 10:34:40 -03: User requested overscan remain black while no-signal area stays gray; added `OVERSCAN_COLOR_RGB565=0x0000` and used it for lines outside active MVS window (`mvs_line_u32 >= MVS_HEIGHT`) in `src/video/video_pipeline.c`.
- 2026-02-18 14:05:41 -03: User approved Step 1 baseline-only genlock experiment scaffolding (no clock/timing behavior changes).
- Added compile-time flag `NEOPICO_EXP_BASELINE_TELEMETRY` (default OFF) in `src/CMakeLists.txt`.
- Added low-rate telemetry path in `src/video/video_capture.c` behind `#if NEOPICO_EXP_BASELINE_TELEMETRY`:
  - Tracks input VSYNC count (`g_input_vsync_count`) and output VSYNC count (`video_frame_count`).
  - Reports every 2 seconds: totals, interval deltas, phase (`out-in`), slip estimate (frames/min), and input/output FPS.
- Safe pattern preserved: experiment is flag-gated, off by default, and does not change sysclk/HSTX/ACR/resync logic.
- 2026-02-18 14:08:04 -03: Built baseline telemetry experiment with `NEOPICO_EXP_BASELINE_TELEMETRY=ON`.
- Commands run: `cmake -S . -B build -DNEOPICO_EXP_BASELINE_TELEMETRY=ON` and `cmake --build build -j4` (both successful).
- Verification: `build/CMakeCache.txt` shows `NEOPICO_EXP_BASELINE_TELEMETRY:BOOL=ON`; artifacts present at `build/src/neopico_hd.uf2` and `build/src/neopico_hd.elf`.
- Next safe step: flash UF2 and collect `[baseline]` serial logs for 2-3 minutes before any timing-control changes.
- 2026-02-18 14:08:04 -03: Rebuilt `neopico_hd` with baseline telemetry enabled; build succeeded.
- Attempted flash via `picotool`: `info`, `load -f -u -v -x build/src/neopico_hd.uf2`, and `info -f` all failed with "No accessible RP-series devices in BOOTSEL mode were found."
- Host also shows no `/dev/tty.usbmodem*`; likely no board USB enumeration/connection at command time.
- Blocker is hardware connection/state, not build output.
- 2026-02-18 14:09:23 -03: User requested using `pi flash`.
- Ran `pi flash` (no args): script usage confirmed (`flash.sh <filename.uf2>`).
- Ran `pi flash build/src/neopico_hd.uf2`: script executed retry sequence (reboot-to-bootloader + load attempts) but failed each attempt with "No accessible RP-series devices in BOOTSEL mode were found."
- Conclusion unchanged: host cannot currently see the RP board in BOOTSEL/application USB mode.
- 2026-02-18 14:22:18 -03: User requested serial telemetry from `menu_diag_experiment_tick_background()` instead of capture loop.
- Removed baseline telemetry code from `src/video/video_capture.c` (no `printf` or telemetry bookkeeping on Core 0 capture path).
- Added baseline telemetry print path to `src/experiments/menu_diag_experiment.c` behind `NEOPICO_EXP_BASELINE_TELEMETRY`:
  - Runs in Core 1 background task (`menu_diag_experiment_tick_background()`), 1 Hz interval.
  - Uses `video_capture_get_frame_count()` for input and `video_frame_count` for output.
  - Prints only when `tud_cdc_connected()`; resets baseline state on disconnect.
- Build verification: `cmake --build build -j4` succeeded.
- 2026-02-18 14:24:05 -03: Cleanup pass after telemetry move: removed now-unused `<stdio.h>` include from `src/video/video_capture.c` and rebuilt.
- Verification: `cmake --build build -j4` succeeded with telemetry now only in Core1 background task path.
- 2026-02-18 14:22:42 -03: Correction: previous entry timestamp text should match shell time 14:22:42 -03.
- 2026-02-18 14:23:32 -03: Re-enabled telemetry flag after moving print path: `cmake -S . -B build -DNEOPICO_EXP_BASELINE_TELEMETRY=ON` + rebuild succeeded.
- Current behavior: baseline serial telemetry now emitted only from `menu_diag_experiment_tick_background()` (Core1 background), not from capture loop.
- 2026-02-18 14:23:08 -03: Correction: previous entry timestamp text should match shell time 14:23:08 -03.
- 2026-02-18 14:23:08 -03: User requested rebuild; ran `cmake --build build -j4` and build succeeded (`neopico_hd` up to date).
- 2026-02-18 14:23:48 -03: Correction: previous line timestamp text should be 14:23:48 -03.
- 2026-02-18 14:28:05 -0300: Switched baseline telemetry from USB serial output to OSD-only updates in Core1 background task.
- : removed CDC/printf path; keeps 1 Hz metrics computation and now calls  only when OSD is visible.
- : added ; reserved bottom rows for IN/OUT FPS and PH/SLIP display.
- Safety intent preserved: no telemetry work in Core0 capture loop; no serial processing required.
- Verification: [  0%] Built target neopico_hd_i2s_capture_pio_h
[  2%] Built target neopico_hd_video_capture_pio_h
[  2%] Built target bs2_default
[  4%] Built target bs2_default_library
[ 40%] Built target pico_hdmi
[100%] Built target neopico_hd succeeded.
- 2026-02-18 14:28:13 -0300: Switched baseline telemetry from USB serial output to OSD-only updates in Core1 background task.
- src/experiments/menu_diag_experiment.c: removed CDC/printf path; keeps 1 Hz metrics computation and now calls selftest_layout_set_baseline(...) only when OSD is visible.
- src/osd/selftest_layout.h and src/osd/selftest_layout.c: added selftest_layout_set_baseline(...); reserved bottom rows for IN/OUT FPS and PH/SLIP display.
- Safety intent preserved: no telemetry work in Core0 capture loop; no serial processing required.
- Verification: cmake --build build -j4 succeeded.
- 2026-02-18 14:28:37 -0300: Note: one earlier SCRATCHBOOK append produced malformed lines due shell quoting while logging; subsequent entries supersede it. No source-code behavior change resulted from that logging error.
- 2026-02-18 14:35:30 -03: User requested lighter-weight/stabler baseline OSD telemetry.
- Minimal change in `src/experiments/menu_diag_experiment.c`: increased baseline update window from 1s to 10s (`BASELINE_REPORT_INTERVAL_US` 1,000,000 -> 10,000,000).
- Rationale: lower update overhead and reduced quantization noise in `slip_fpm`/FPS metrics.
- Verification: `cmake --build build -j4` succeeded.
- 2026-02-18 14:32:47 -03: Correction: previous entry timestamp text should be 14:32:47 -03.
- 2026-02-18 14:39:13 -0300: Implemented static genlock experiment (compile-time, off by default) and built test config with it ON.
- : added  (BOOL) and  (STRING, default 5920) compile options.
- : boot-time sysclk selection uses target FPS when static genlock enabled; computes sysclk from . Added one-time  after video pipeline init.
- Test build config used now: , , .
- Verification: -- Configuring done (0.1s)
-- Generating done (0.1s)
-- Build files have been written to: /Users/dudu/Projects/neogeo/neopico-hd/build + [  0%] Built target neopico_hd_i2s_capture_pio_h
[  1%] Built target neopico_hd_video_capture_pio_h
[  2%] Built target bs2_default
[  4%] Built target bs2_default_library
[ 40%] Built target pico_hdmi
[100%] Built target neopico_hd succeeded.
- 2026-02-18 14:39:32 -0300: Implemented static genlock experiment (compile-time, OFF by default) and built test config with it ON.
- src/CMakeLists.txt: added options NEOPICO_EXP_GENLOCK_STATIC (BOOL) and NEOPICO_GENLOCK_TARGET_FPS_X100 (STRING, default 5920).
- src/main.c: boot-time sysclk uses target FPS when static genlock is enabled; formula 126000 * fps / 60. Added one-time video_output_update_acr() after video_pipeline_init().
- Active build config now: NEOPICO_EXP_GENLOCK_STATIC=ON, NEOPICO_GENLOCK_TARGET_FPS_X100=5920, NEOPICO_EXP_BASELINE_TELEMETRY=ON.
- Verification: cmake configure + build completed successfully.
- Note: previous malformed SCRATCHBOOK lines in this section were logging artifacts from shell quoting; this entry supersedes them.
- 2026-02-18 14:46:23 -0300: Added lightweight frequency-isolation toggles to pico_hdmi bouncing_box example.
- lib/pico_hdmi/examples/bouncing_box/CMakeLists.txt: added EXP_SYSCLK_KHZ and EXP_DVI_ONLY options, wired into compile definitions.
- lib/pico_hdmi/examples/bouncing_box/main.c: uses EXP_SYSCLK_KHZ for set_sys_clock_khz(), sets video_output_set_dvi_mode() from EXP_DVI_ONLY, and skips audio generation/melody when DVI-only is enabled.
- Built and verified custom isolation config: EXP_SYSCLK_KHZ=124290, EXP_DVI_ONLY=ON.
- 2026-02-18 14:47:42 -03: Implemented fixed-pixel-clock V-total match experiment for jitter reduction without lowering TMDS clock.
- Added compile-time flags in src/CMakeLists.txt: NEOPICO_EXP_VTOTAL_MATCH (BOOL, default OFF) and NEOPICO_EXP_VTOTAL_LINES (STRING, default 532).
- In src/main.c, when V-total match is enabled, code builds a custom mode from 480p baseline and sets v_total_lines and v_back_porch before video init via video_output_set_mode().
- Static sysclk-genlock path is automatically bypassed when V-total match is enabled (keeps sysclk at 126000).
- Active build now: NEOPICO_EXP_VTOTAL_MATCH=ON, NEOPICO_EXP_VTOTAL_LINES=532, NEOPICO_EXP_GENLOCK_STATIC=OFF, NEOPICO_EXP_BASELINE_TELEMETRY=ON.
- Expected output frame rate at current timing: 25.2 MHz / (800 * 532) = 59.210526 Hz.
- Verification: configure + build succeeded.
- 2026-02-18 15:03:16 -0300: User requested revert of lib/pico_hdmi changes. Reverted example-only edits in submodule (, ); submodule no longer dirty in top-level status.
- 2026-02-18 15:03:21 -0300: User requested revert of lib/pico_hdmi changes. Reverted example-only edits in submodule (bouncing_box CMakeLists and main files); submodule is clean in top-level status.
- 2026-02-18 15:03:21 -0300: Note: prior SCRATCHBOOK append attempt in this turn had shell quoting issues and did not alter source files.
- 2026-02-18 15:04:21 -0300: Built stock firmware configuration on user request.
- Configure flags used: NEOPICO_EXP_GENLOCK_STATIC=OFF, NEOPICO_EXP_VTOTAL_MATCH=OFF, NEOPICO_EXP_BASELINE_TELEMETRY=OFF.
- Build result: cmake configure + build succeeded; neopico_hd.uf2 ready.
- 2026-02-18 15:06:05 -0300: Gated baseline OSD template rows behind NEOPICO_EXP_BASELINE_TELEMETRY in src/osd/selftest_layout.c.
- With telemetry flag OFF, IN/OUT and PH/SLIP rows are no longer rendered; selftest_layout_set_baseline() becomes a no-op.
- Verified by rebuilding stock config (GENLOCK_STATIC=OFF, VTOTAL_MATCH=OFF, BASELINE_TELEMETRY=OFF): build succeeded.
- 2026-02-19 20:35:54 -0300: Added DARK indicator slot to self-test OSD and abbreviated labels to fit: CSYN, PCLK, SHDW, DARK.
- Added SELFTEST_BIT_DARK and included it in SELFTEST_VIDEO_BITS_MASK.
- menu_diag_experiment samples DARK only when PIN_MVS_DARK is defined; current hardware without that define leaves DARK neutral/fail state based on renderer.
- Verification: cmake --build build -j4 succeeded.
- 2026-02-19 20:37:07 -0300: User requested color bit rows moved down by one. Updated selftest layout constants: RED/GREEN/BLUE rows shifted +1 (7/8/9 -> 8/9/10).
- Verification: cmake --build build -j4 succeeded.
- 2026-02-19 20:38:33 -0300: Follow-up OSD layout tweak: moved bit headers and audio section down one row to match earlier color-row shift.
- Updated constants in selftest_layout.c: ST_BITS_HEADER_ROW 6->7, ST_AUDIO_ROW 11->12, ST_AUDIO_LABEL_ROW 12->13.
- Verification: cmake --build build -j4 succeeded.
