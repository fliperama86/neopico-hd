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
- 2026-02-19 20:45:19 -0300: OSD label fit tweak: changed CSYN->CSYNC and DARK->DRK in selftest layout; moved CSYNC icon one column right to avoid overlap.
- Verification: cmake --build build -j4 succeeded.
- 2026-02-19 20:47:18 -0300: Adjusted video-status label/icon spacing: shifted PCLK, SHDW, and DRK label+icon columns +1 to tighten DRK/icon gap.
- Updated constants in selftest_layout.c: PCLK 8/12->9/13, SHDW 15/19->16/20, DRK 22/26->23/27.
- Verification: cmake --build build -j4 succeeded.
- 2026-02-19 20:48:43 -0300: Fixed persistent DRK/icon gap by shifting only DRK label one additional column right (ST_DARK_COL 23->24) while keeping icon at 27.
- Verification: cmake --build build -j4 succeeded.
- 2026-02-19 20:49:35 -0300: User requested direct icon shift only: moved DRK icon left one column (ST_DARK_ICON_COL 27->26), no other layout changes.
- Verification: cmake --build build -j4 succeeded.
- 2026-02-19 21:55:30 -0300: User reported DARK/SHADOW path CPU cost causing vertical jitter and requested maximum optimization.
- `src/video/video_capture.c`: replaced per-pixel SHADOW branch + channel correction in `convert_pixel()` with a precomputed branchless 64K capture LUT (`g_capture_lut`) indexed by raw bits `[17:2]`.
- LUT generation now bakes: raw mask/reverse handling, per-channel correction, non-shadow black-level clamp, and SHADOW->(halve + DARK) behavior to preserve prior output math while removing hot-loop work.
- Non-DARK/SHADOW build path remains separate with existing 32K LUT (`g_color_correct_lut`) to avoid unnecessary default-memory growth.
- Verification: built successfully in isolated dirs for both configs:
  - `cmake -S . -B /tmp/neopico_dscheck -DNEOPICO_ENABLE_DARK_SHADOW=ON && cmake --build /tmp/neopico_dscheck -j4`
  - `cmake -S . -B /tmp/neopico_defaultcheck -DNEOPICO_ENABLE_DARK_SHADOW=OFF && cmake --build /tmp/neopico_defaultcheck -j4`
- 2026-02-19 21:56:16 -0300: Additional hot-loop optimization for capture conversion.
- `src/video/video_capture.c`: added `convert_active_pixels(...)` and switched both active-line loops to call it.
- DARK/SHADOW path now uses 4-pixel unrolled LUT reads per iteration (plus tail loop), minimizing loop overhead in Core0 capture path.
- Re-verified both build variants after this loop change:
  - `cmake --build /tmp/neopico_dscheck -j4`
  - `cmake --build /tmp/neopico_defaultcheck -j4`
- 2026-02-19 22:08:03 -0300: User requested DARK/SHADOW enabled by default; updated CMake default in `src/CMakeLists.txt`.
- Changed `option(NEOPICO_ENABLE_DARK_SHADOW ... OFF)` to `... ON` so fresh configure defaults to enabled while still allowing explicit override with `-DNEOPICO_ENABLE_DARK_SHADOW=OFF`.
- 2026-02-19 22:10:42 -0300: User requested rebuild; ran `cmake --build build -j4` in repo root.
- Reconfigure + build succeeded; artifacts updated: `build/src/neopico_hd.uf2` and `build/src/neopico_hd.elf`.
- 2026-02-19 22:20:06 -0300: Investigated SHDW self-test false negatives; likely cause was Core1 GPIO polling missing sparse/short SHADOW pulses.
- Added Core0-captured SHADOW activity latch in `src/video/video_capture.c`:
  - New `g_shadow_activity_epoch` increments once per input frame if any raw pixel had bit17=1.
  - `convert_active_pixels(...)` now returns per-line shadow-seen flag using OR of raw capture words.
  - New API `video_capture_get_shadow_activity_epoch()` exposed in `src/video/video_capture.h`.
- Updated self-test sampler in `src/experiments/menu_diag_experiment.c` to set `SELFTEST_BIT_SHADOW` when `video_capture_get_shadow_activity_epoch()` changes, replacing direct `gpio_get(PIN_MVS_SHADOW)` polling.
- Verification: `cmake --build build -j4` succeeded.
- 2026-02-19 22:39:12 -0300: User reported prior SHADOW latch test caused sync drop and reverted it; requested safer mapping test.
- Added flag-gated test path `NEOPICO_EXP_SHADOW_BITSCAN` (default OFF) in `src/CMakeLists.txt` and enabled it for current build (`build/CMakeCache.txt: NEOPICO_EXP_SHADOW_BITSCAN=ON`).
- `src/video/video_capture.c`: added ultra-light sparse sampler for raw bits 15..17 (`SHADOW_BITSCAN_STRIDE=20`) and sticky seen-mask getter `video_capture_get_shadow_bits_seen_mask()`.
- `src/experiments/menu_diag_experiment.c`: added OSD probe line `S15/S16/S17` on row 0 and wired SHDW icon from sampled raw bit17; with no DARK pin, DRK icon temporarily mirrors sampled raw bit16 for mapping diagnostics.
- Build verification: `cmake -S . -B build -DNEOPICO_EXP_SHADOW_BITSCAN=ON && cmake --build build -j4` succeeded; artifacts updated.
- 2026-02-19 23:04:27 -0300: User test result with bitscan OSD: `S15=1, S16=1, S17=0` (sticky highs).
- Interpretation: raw capture bit17 never observed high in current test scenes; bits15/16 being high is expected from red data activity.
- Hardware-file cross-check:
  - `hardware/neopico-hd/neopico-hd.kicad_pcb` shows `GPIO44` on net `SHD` (pad 50), and `GPIO45` explicitly unconnected (`unconnected-(A1-GPIO45-Pad51)`).
  - `MVS_SHD` enters level shifter U4 on `A8` and exits as `SHD` on `B8`, indicating intended non-inverting path to `GPIO44`.
- Current hypothesis: SHD line at Pico side is stuck low / never asserted in captured data (not an off-by-one GPIO mapping issue between 44/45).
- 2026-02-19 23:19:24 -0300: User requested disabling bitscan experiment and rebuilding.
- Ran: `cmake -S . -B build -DNEOPICO_EXP_SHADOW_BITSCAN=OFF && cmake --build build -j4`.
- Verification:
  - `build/CMakeCache.txt` => `NEOPICO_EXP_SHADOW_BITSCAN:BOOL=OFF`
  - Updated artifacts: `build/src/neopico_hd.uf2`, `build/src/neopico_hd.elf`
- 2026-02-19 23:24:41 -0300: User requested cross-check against MiSTer/MAME shadow-dark behavior.
- Reference repos under `~/Projects/references`:
  - `NeoGeo_MiSTer` at `2325e6c`
  - `mame` at `e47c0f3` (sparse checkout of Neo Geo files)
- MiSTer core (`neogeo.sv` + `rtl/io/syslatch.v`):
  - `SHADOW` is `syslatch` bit0 (`SLATCH[0]`) and is wired as a global control signal.
  - DARK effect comes from palette bit15 subtraction in RGB expansion (`PAL_RAM_REG[15]`).
  - SHADOW halves output RGB after DARK expansion (`~SHADOW ? R8 : {1'b0, R8[7:1]}` etc).
- MAME Neo Geo (`src/mame/neogeo/neogeo_v.cpp` + `neogeo.cpp`):
  - Uses resistor-model lookups for 4 cases: normal, dark, shadow, dark+shadow.
  - Dark is palette bit15; shadow is system latch q0 (`set_screen_shadow`).
  - Rendering selects shadow pen bank (`+0x2000`) when screen shadow is active; not modeled as per-pixel SHADOW pin sampling.
- 2026-02-19 23:34:44 -0300: Improved selftest SHDW/DRK behavior to reduce false negatives on low-frequency SHADOW.
- `src/experiments/menu_diag_experiment.c`:
  - Added SHADOW activity hold (`SELFTEST_SHADOW_HOLD_UPDATES=30`) so SHDW stays green for ~30 update windows after any observed SHADOW assertion.
  - Removed temporary DRK mapping to raw bit16 under `NEOPICO_EXP_SHADOW_BITSCAN` for boards without DARK pin.
- `src/osd/selftest_layout.c`:
  - DRK icon now renders neutral (`-`) when `PIN_MVS_DARK` is not defined (current board), instead of appearing as failed/inactive.
- Build verification: `cmake --build build -j4` succeeded; updated `build/src/neopico_hd.elf` (and normal extra outputs).
- 2026-02-19 23:49:07 -0300: User requested MiSTer-reference DARK/SHADOW behavior under existing flag only, plus cleanup of obsolete flags.
- Removed obsolete diagnostic flag path `NEOPICO_EXP_SHADOW_BITSCAN` from:
  - `src/CMakeLists.txt` (option + compile define removed)
  - `src/video/video_capture.c/.h` (bitscan state/API removed)
  - `src/experiments/menu_diag_experiment.c` (probe overlays and capture-mask dependency removed)
- `ENABLE_DARK_SHADOW` path migrated to MiSTer-style composition:
  - DARK and SHADOW are independent (no "SHADOW forces DARK").
  - SHADOW is 50% output halving.
  - DARK uses MiSTer-compatible per-channel dark step model.
- Added optional DARK wiring support with no new feature flag:
  - `src/mvs_pins.h`: optional `PIN_MVS_DARK 45` + `MVS_CAPTURE_BITS`/`MVS_CAPTURE_PIN_LAST`.
  - `src/video/video_capture.pio`: added `mvs_pixel_capture_dark19` program (`in pins, 19`) for GP27-45 capture.
  - `src/video/video_capture.c`: selects 18-bit vs 19-bit PIO program/config from `PIN_MVS_DARK`.
- Memory/safety adjustment:
  - Attempted 131072-entry LUT caused RAM overflow at link time.
  - Final implementation uses 65536-entry LUT (RGB+SHADOW, DARK=0 fast path) plus MiSTer-compatible runtime conversion only when DARK bit is asserted.
  - This keeps current build stable while enabling future DARK-wire validation.
- Reconfigure/build verification:
  - `cmake -S . -B build && cmake --build build -j4` (initially exposed RAM overflow before LUT downsizing)
  - `cmake --build build -j4` after downsizing succeeded.
  - Cleared stale cache entry for removed option: `cmake -U NEOPICO_EXP_SHADOW_BITSCAN -S . -B build`.
- 2026-02-19 23:53:18 -0300: User requested treating DARK+SHADOW as one feature and removing "bodge/optional DARK" framing.
- Updated mapping/docs to standard 19-bit capture (GP27-45) with DARK as normal input:
  - `src/mvs_pins.h`: `PIN_MVS_DARK` now defined directly; `MVS_CAPTURE_BITS=19` fixed.
  - `src/video/video_capture.c`: removed `#ifdef PIN_MVS_DARK` branching; always uses 19-bit capture program/config.
  - `src/video/video_capture.pio`: removed obsolete 18-bit capture program; retained single 19-bit capture program.
  - `src/experiments/menu_diag_experiment.c`: always samples DARK for selftest.
  - `src/osd/selftest_layout.c`: DRK icon always rendered from toggled DARK bit.
- Removed wording about "no DARK pin", "optional DARK", and "bodge wire" from:
  - `README.md`
  - `docs/MVS_MV1C_DIGITAL_VIDEO.md`
  - `src/video/hardware_config.h`
- Build verification: `cmake --build build -j4` succeeded.
- 2026-02-19 23:56:09 -0300: User reported signal drops after DARK integration changes.
- Stabilization pass in `src/video/video_capture.c`:
  - Replaced expensive per-dark-pixel full raw->RGB reconstruction path with LUT-first conversion plus lightweight RGB565 dark-step adjustment (`rgb565_apply_dark_step`).
  - Kept single DARK+SHADOW feature path and 19-bit capture mapping.
  - Goal: recover Core0 headroom and avoid capture/sync drops.
- Build verification: `cmake --build build -j4` succeeded.
- 2026-02-19 23:59:13 -0300: User reported overhead still too high after first stabilization.
- Additional hot-loop optimization in `src/video/video_capture.c`:
  - Added tiny precomputed DARK step tables (`g_dark5_lut`, `g_dark6_lut`) to avoid per-channel branch/arithmetic in dark adjust.
  - Reworked `convert_active_pixels(...)` to a 4-pixel fast path:
    - Always does branchless LUT fetches first.
    - Applies DARK adjustment only when any of the 4 raw pixels has DARK bit set.
  - Tail loop likewise uses LUT-first + conditional DARK adjust (no full convert path call).
- Build verification: `cmake --build build -j4` succeeded.
- 2026-02-20 00:01:57 -0300: User requested reverting performance-costly DARK runtime approach to previous less-accurate fast path.
- `src/video/video_capture.c` reverted to pure LUT hot-loop conversion:
  - Removed per-pixel DARK runtime adjustment in `convert_pixel()` and `convert_active_pixels()`.
  - Removed DARK step helper tables and generation.
  - Conversion now always indexes LUT with raw bits [17:2], effectively ignoring DARK in conversion for maximum stability/headroom.
- Kept 19-bit capture/pin mapping and single DARK+SHADOW feature framing as requested.
- Build verification: `cmake --build build -j4` succeeded.
- 2026-02-20 00:05:15 -0300: User requested docs update to reflect current stable implementation.
- Updated documentation to match current behavior (performance-first conversion):
  - `README.md`: adjusted feature/status wording; removed claims of full DARK output accuracy and old 256KB/interp wording.
  - `docs/MVS_MV1C_DIGITAL_VIDEO.md`: kept 19-bit capture map, documented SHADOW conversion path and that DARK is captured but not currently applied in output conversion.
  - `docs/ARCHITECTURE.md`: replaced old Interpolator/256KB statements with 64K LUT hot-path notes.
  - `docs/STREAMING_PIPELINE_PLAN.md`: replaced stale "future DARK/SHADOW" section with current status.
  - `docs/HSTX_IMPLEMENTATION.md`: updated Core0 conversion summary wording.
  - `docs/How SHADOW and DARK Work on Neo Geo.MD`: added explicit firmware note to avoid mismatch vs reference behavior.
- 2026-02-20 00:05:46 -0300: Follow-up doc consistency fix in `docs/How SHADOW and DARK Work on Neo Geo.MD`.
- Replaced outdated statement "SHADOW forces DARK on" with wording that SHADOW and DARK are independent controls (matching current/reference behavior notes).
- 2026-02-20 00:15:46 -0300: User requested CMake flag cleanup and DARK/SHADOW default change.
- `src/CMakeLists.txt` updates:
  - Removed `NEOPICO_EXP_MENU_DIAG` option and hardcoded `EXP_MENU_DIAG=1` compile define.
  - Removed `NEOPICO_EXP_BASELINE_TELEMETRY` option and hardcoded `NEOPICO_EXP_BASELINE_TELEMETRY=0`.
  - Removed `HSTX_LAB_BUILD` compile define.
  - Removed `hardware_interp` from target link libraries (no remaining usage).
  - Changed `NEOPICO_ENABLE_DARK_SHADOW` option default from `ON` to `OFF`.
- Reconfigured build cache to drop stale removed options and align current build with new default:
  - `cmake -U NEOPICO_EXP_MENU_DIAG -U NEOPICO_EXP_BASELINE_TELEMETRY -S . -B build -DNEOPICO_ENABLE_DARK_SHADOW=OFF`
  - `build/CMakeCache.txt` now shows `NEOPICO_ENABLE_DARK_SHADOW:BOOL=OFF`.
- Build verification: `cmake --build build -j4` succeeded.
- 2026-02-20 00:19:43 -0300: User asked if dead code was removed; performed full dead-branch cleanup for removed flags.
- Removed compile-time dead branches/code tied to `EXP_MENU_DIAG` and `NEOPICO_EXP_BASELINE_TELEMETRY`:
  - `src/experiments/menu_diag_experiment.c`: removed all `#if EXP_MENU_DIAG` and baseline telemetry conditional blocks; file now always compiles active path.
  - `src/experiments/menu_diag_experiment.h`: removed fallback `#ifndef EXP_MENU_DIAG`.
  - `src/osd/selftest_layout.c/.h`: removed baseline telemetry macros, rows, `selftest_layout_set_baseline(...)` implementation, and declaration.
  - `src/CMakeLists.txt`: removed now-unneeded compile defines `EXP_MENU_DIAG=1` and `NEOPICO_EXP_BASELINE_TELEMETRY=0`.
- Verification:
  - `rg` confirms no remaining references to `EXP_MENU_DIAG`, `NEOPICO_EXP_BASELINE_TELEMETRY`, `HSTX_LAB_BUILD`, or `hardware_interp` in touched source/build config.
  - Reconfigure/build succeeded with DARK/SHADOW default OFF:
    - `cmake -S . -B build -DNEOPICO_ENABLE_DARK_SHADOW=OFF && cmake --build build -j4`.
- 2026-02-20 00:27:39 -03: User requested main-branch DARK/SHADOW algorithm while keeping DARK pin mapping on GPIO45. Updated src/video/video_capture.c to main-style shadow-dark LUT math; retained 19-bit capture path/pin map. Rebuild passed (cmake --build build -j4).
- 2026-02-20 00:32:29 -03: Reviewed and corrected staged docs for DARK/SHADOW accuracy. Synced docs to current implementation: 19-bit capture incl. GPIO45 DARK, default NEOPICO_ENABLE_DARK_SHADOW=OFF (32K RGB LUT), optional ON path uses 64K RGB+SHADOW LUT with legacy shadow math; DARK shown in diagnostics but not used as conversion input.
- 2026-03-24 15:17: User requested "Build and flash the 240p mode" and asked to proceed without repeated permission prompts.
- Method used: configured isolated `build-240p-env` with `CFLAGS/CXXFLAGS=-DVIDEO_MODE_320x240=1`, built target `neopico_hd`, then flashed `build-240p-env/src/neopico_hd.uf2` via `pi flash`.
- Blockers/workarounds:
  - Initial attempt with `-DCMAKE_C_FLAGS/-DCMAKE_CXX_FLAGS` dropped toolchain arch flags (`-mcpu` etc.), causing RP2350 spin-lock compile errors.
  - 240p path compile typo fixed in `lib/pico_hdmi/src/video_output_rt.c`: `VIDEO_MODE_240P` -> `video_mode_240_p`.
- Result: build and flash succeeded; device rebooted into application mode after UF2 load.
- 2026-03-24 15:41: User reported 240p implementation is visually broken (half image + tearing) and requested a fact-based deep investigation and solid plan before further edits.
- Investigation findings (code-verified):
  - `src/video/video_pipeline.c` still hardcodes 480p assumptions in scanline callback (`fb_line = active_line >> 1`, 2x blitter only), even when 240p mode is selected.
  - In 240p mode (`1280x240`), callback currently writes only half the required horizontal words and maps only ~half the source vertical range, matching observed symptom.
  - OSD span math is also tied to 2x horizontal scaling and must be mode-aware for 4x pixel replication.
  - Timing mismatch noted: 240p mode table uses `v_total=262` (~60.11 Hz at 25.2 MHz), while MVS source is ~59.18 Hz; without lock control this increases frame-phase churn and can amplify visible tearing.
- Resource snapshot from built 240p ELF:
  - `text=42144`, `bss=308888`.
  - Large RAM users: `g_line_ring` 163856, `g_color_correct_lut` 65536, `di_ring_buffer` 36864, `g_dma_buffer` 16384, `g_line_buffers` 3072, `line_buffer` 2560.
- Next action pending user approval: implement mode-correct scanline mapping + full-line write guarantees first, then instrument and validate timing/tearing metrics before any genlock/timing tuning.
- 2026-03-24 15:49: Implemented mode-aware scanline/OSD scaling fix in `src/video/video_pipeline.c`.
- Changes:
  - Switched to runtime mode API include (`pico_hdmi/video_output_rt.h`) and cached active mode geometry at init/VSYNC.
  - Added `video_pipeline_refresh_mode_cache()` and cached fields (`mode_is_240p`, `mode_h_words`, OSD word spans).
  - In scanline callback:
    - 480p path keeps `fb_line = active_line >> 1` + 2x blit.
    - 240p path uses `fb_line = active_line` + 4x blit to fully populate 1280 active pixels.
    - No-signal and OSD branches now use mode-correct word offsets and full-line writes.
- Verification:
  - Built successfully in both default and 240p configs:
    - `cmake --build build --target neopico_hd`
    - `cmake --build build-240p-env --target neopico_hd`
  - Flashed updated 240p UF2 (`build-240p-env/src/neopico_hd.uf2`) via `pi flash`; device rebooted into application mode.
- 2026-03-24 15:54: User confirmed the 240p fix is visually perfect on hardware.
- Cleanup completed on request: removed temporary `build-240p` and `build-240p-env` directories.
- Current intentional working-tree changes for this effort: `src/video/video_pipeline.c`, `lib/pico_hdmi` (runtime-mode symbol fix), and `SCRATCHBOOK.md`.
- 2026-03-24 16:02: User requested CI artifact for 240p mode.
- Updated `.github/workflows/build.yml`:
  - Added dedicated CI build step `Build NeoPico-HD (240p mode)` in `build-240p`.
  - Uses compile-time mode define via env flags during configure: `CFLAGS/CXXFLAGS=-DVIDEO_MODE_320x240=1`.
  - Renames outputs to `neopico_hd_240p.uf2` and `neopico_hd_240p.elf`.
  - Added both files to `Upload Artifacts` paths so they are included in `neopico-hd-build` artifact and downstream release packaging.
- 2026-03-24 16:10: User reported `pico_hdmi` submodule dirty; migrated 240p selection to top-level firmware option so submodule can stay clean.
- `src/CMakeLists.txt`:
  - Added `NEOPICO_VIDEO_240P` option (default OFF).
  - Propagates `NEOPICO_VIDEO_240P` compile definition only to `neopico_hd` target.
- `src/main.c`:
  - Added fallback macro for `NEOPICO_VIDEO_240P`.
  - Before `video_pipeline_init()`, selects runtime mode with `video_output_set_mode(&video_mode_240_p)` when flag is ON.
  - Keeps existing `NEOPICO_EXP_VTOTAL_MATCH` path as alternate mode select.
- `.github/workflows/build.yml`:
  - 240p CI build now uses `-DNEOPICO_VIDEO_240P=ON` (removed global `CFLAGS/CXXFLAGS=-DVIDEO_MODE_320x240=1`).
- Submodule cleanup:
  - Reverted local change in `lib/pico_hdmi/src/video_output_rt.c`; submodule is clean again.
- Verification:
  - Built default (`NEOPICO_VIDEO_240P=OFF`) and 240p (`NEOPICO_VIDEO_240P=ON`) successfully.
- 2026-03-24 16:16: User requested disabling auto-release on every merge.
- Updated `.github/workflows/build.yml` release behavior:
  - `release` job now runs only on tag refs matching `v*` (`if: startsWith(github.ref, 'refs/tags/v')`).
  - Removed main-branch auto-bump/tag creation logic.
  - Release version now always uses pushed tag name (`github.ref_name`).
- Resulting policy: merges to `main` still build/upload artifacts, but GitHub Releases are published only when manually pushing a version tag.
- 2026-03-24 16:22: User requested a local shell script to automate tag-based releases.
- Added executable `scripts/release.sh`:
  - Enforces clean working tree.
  - Accepts optional explicit semver tag `vX.Y.Z`; otherwise auto-bumps patch from latest `v*` tag.
  - Validates tag format and checks local/remote tag collisions.
  - Creates annotated tag and pushes it to `origin` (triggering the tag-only GitHub release workflow).
  - Includes `--help` usage output.
- Portability tweak: script uses standard `grep/head` (no `rg` dependency at runtime).
- 2026-04-23: User porting upstream 720p60 (VIC 4) support from `video_output.c` into `video_output_rt.c`. Blocker: adding a single `#include "pico_hdmi/video_output.h"` to `video_output_rt.c` silently kills the HDMI signal even though no symbol from that header is used and symbol-table *names* are identical between working/broken ELFs.
- Strong candidate cause: flash cache-line alignment shift for Core 1 ISR code (same class of regression as previously recorded — adding compilation units / reordering code pushes hot functions across 32-byte XIP cache-line boundaries → HSTX FIFO underrun).
- Recommended next-time investigation order (cheapest first):
  - Compare `arm-none-eabi-nm --numeric-sort` addresses (not just names) for both ELFs. Focus on Core 1 HSTX DMA IRQ, `video_output_rt` scanline/vactive ISR, and `build_line_with_di`-equivalent in rt path. Any address shift = alignment is the cause.
  - Check `(addr & 0x1F)` for those hot symbols — a straddling function in the broken build explains underrun.
  - If alignment-caused: pin the fragile function with `__scratch_x("")` / linker time-critical section so flash position no longer matters.
  - Only if addresses are actually unchanged: suspect a macro side-effect from the header (`MODE_H_TOTAL_PIXELS`, `DI_HSYNC_ACTIVE`, etc.) flipping a downstream conditional — test via a no-op include of an empty header to isolate positional vs semantic.
  - Independent sanity check: add a Core 0 serial print in `init_rt_from_mode` to confirm it ran with expected values before Core 1 launches (expected to be fine; confirms failure mode is post-init timing, not init-path).
- 2026-04-24 00:59:38 -0300: User reported current 720p branch state.
- `lib/pico_hdmi` is committed at `385e954` with 720p60 rt-path support gated by `-DVIDEO_MODE_1280x720` on `pico_hdmi`.
- `neopico-hd` has uncommitted `src/CMakeLists.txt` and `src/main.c` changes adding `NEOPICO_VIDEO_720P`, setting vreg 1.30V, sysclk 372 MHz, and selecting `video_mode_720_p`; no `video_pipeline.c` changes yet.
- Hardware results: default 480p has no regression; 720p establishes HDMI link as 1280x720@60.
- 720p symptoms: image confined to top-left about 640x448, rest garbage/black; image scrolls upward about once per second.
- Current cause model: `video_pipeline.c` still has 480p assumptions (`mode_scale_fn` 2x, `fb_line = active_line >> 1`), so it fills only half a 1280-pixel line and maps 224 MVS lines into active lines 16..463. Scroll likely frame-rate beat: HDMI at 372 MHz is about 60.12 Hz vs MVS about 59.18 Hz, beat about 1.06 s.
- Pending work: 720p pipeline scaling needs 4x horizontal and 3x vertical with letterbox; `audio_subsystem.c` still passes hardcoded `true` instead of `DI_HSYNC_ACTIVE` for Data Island placement; frame-rate lock between MVS and 720p60 is unresolved.
- 2026-04-24 01:07:53 -0300: User approved code changes and flashing firmware for 720p fix. Preference: preserve 4:3 aspect ratio with letterbox. Implementation target: scale full 320x240 canvas to 1280x720 (4xH/3xV), leaving MVS 224 active lines as 672 output lines with 24px top/bottom black letterbox.
- 2026-04-24 01:10:42 -0300: Implemented initial 720p fix in `src/video/video_pipeline.c` and `src/audio/audio_subsystem.c`.
- Pipeline now detects 1280x720, uses 4x horizontal scaling, maps active HDMI lines to internal 320x240 canvas with 3x vertical replication, and reuses the previous line buffer for duplicate 720p vertical lines.
- Audio packets generated by `audio_subsystem.c` now use `AUDIO_DI_HSYNC_ACTIVE=false` when `NEOPICO_VIDEO_720P=1`, matching 720p back-porch Data Island placement.
- 2026-04-24 01:13:10 -0300: Verification build passed for default `build` and 720p `build_720p`. Sizes: default `text=42656 bss=309032`; 720p `text=42800 bss=309032`. `git diff --check` passed.
- Flashing geometry/Data-Island fix first while preserving standard 720p timing; frame-rate beat likely remains until a separate vtotal/source-lock experiment.
- 2026-04-24 01:14:07 -0300: Flashed `build_720p/src/neopico_hd.uf2` via `pi flash`; command rebooted RP2350 to BOOTSEL, loaded UF2 successfully, then rebooted into application mode.
- 2026-04-24 01:15:12 -0300: User requested 720p scaling changed to 3x on both axes. Target: 320x240 canvas -> 960x720 centered in 1280x720, with 160px side pillarbox; MVS active 224 lines remains 672px tall with 24px top/bottom letterbox.
- 2026-04-24 01:16:38 -0300: Patched `video_pipeline` for 720p centered 3x. Added `video_pipeline_triple_pixels_fast`; mode cache now computes image width/margins. Captured video/OSD-over-video get black side margins; no-signal fallback remains full-width gray.
- 2026-04-24 01:17:18 -0300: Build/flash for 720p centered 3x passed. `git diff --check` clean. Default and 720p builds passed. Sizes: default `text=42896 bss=309036`; 720p `text=43048 bss=309036`. Flashed `build_720p/src/neopico_hd.uf2` with `pi flash`; device rebooted into application mode.
- 2026-04-24 01:22:21 -0300: User reported 720p has random pixel glitches but acceptable for now. Requested brief README documentation as experimental, then commit and push. Plan: stage 720p firmware/docs only; leave unrelated schematic, menu diag, and standalone selftest changes unstaged.
- 2026-04-24 01:30:21 -0300: User asked likely causes for random 720p pixel glitches.
- Quick symbol check on flashed 720p ELF: `video_pipeline_scanline_callback`, `dma_irq_handler`, and 3x/4x scalers are in SRAM (`0x2008...`), not flash/XIP. `line_buffer` is at `0x2004d318`, only 4-byte aligned.
- Leading hypotheses: 720p HSTX/ISR timing margin is thin at 74.4 MHz pixel clock; extra 3x pillarbox fill/scale passes may occasionally miss DMA/HSTX deadlines; 372 MHz sysclk/74.4 MHz TMDS stresses board/HDMI signal integrity; `line_buffer` alignment/bus contention could matter; audio Data Island generation may add jitter/pressure; frame-rate beat affects scroll/tearing but is less likely to produce isolated random pixels.
- 2026-04-24 01:32:53 -0300: User reported the 720p vertical scroll is gone after centered 3x scaling build. Remaining 720p issue appears isolated to random pixel glitches/sparkles.
- 2026-04-24 01:34:03 -0300: User said to proceed with 720p glitch investigation. First low-risk experiment: align `lib/pico_hdmi/src/video_output_rt.c` HSTX `line_buffer` to 64 bytes, rebuild 720p, flash, and compare random pixel glitches.
- 2026-04-24 01:34:46 -0300: 64-byte `line_buffer` alignment experiment built and flashed. New 720p ELF: `text=43048 bss=309084`; `line_buffer` at `0x20030040` (64-byte aligned). Flashed `build_720p/src/neopico_hd.uf2` successfully via `pi flash`.
- 2026-04-24 01:36:20 -0300: User reported 64-byte `line_buffer` alignment looks the same. Alignment alone is unlikely to be the primary cause. Next isolation target: 720p DVI-only/no-audio build to remove Data Islands and Core 1 audio background load.
- 2026-04-24 01:37:41 -0300: Added temporary `NEOPICO_VIDEO_DVI_ONLY` compile option and main guards: when ON, firmware calls `video_output_set_dvi_mode(true)` and skips Core 1 audio background task. Built with `NEOPICO_VIDEO_720P=ON NEOPICO_VIDEO_DVI_ONLY=ON`; size `text=39320 bss=283232`. Flashed DVI-only 720p UF2 successfully.
- 2026-04-24 01:38:36 -0300: User reported DVI-only/no-audio 720p still has same random glitches. Data Islands/audio load unlikely primary cause. Next isolation: 720p DVI-only Core 1 static test pattern with no MVS line-ring reads and no scaler work.
- 2026-04-24 01:39:39 -0300: Added temporary `NEOPICO_VIDEO_TEST_PATTERN` option. In 720p test-pattern builds, scanline callback fills static horizontal color bars once at active line 0 and then returns for all lines, removing capture/ring/scaler/per-line rendering from the active display path.
- 2026-04-24 01:40:28 -0300: Built and flashed 720p DVI-only static test pattern. Build flags: `NEOPICO_VIDEO_720P=ON`, `NEOPICO_VIDEO_DVI_ONLY=ON`, `NEOPICO_VIDEO_TEST_PATTERN=ON`. Size `text=39432 bss=283232`. Expected display: full-height static horizontal color bars, no game video/audio.
- 2026-04-24 01:41:53 -0300: User reported static 720p DVI test pattern is crystal clear. This points away from HDMI/HSTX electrical basics and toward live render/scaler/source-read timing. Next test: DVI-only synthetic 3x scaler path from an in-RAM 320px source line, no MVS line-ring reads.
- 2026-04-24 01:42:52 -0300: Built and flashed 720p DVI-only synthetic 3x scaler test. Same flags as prior test (`720P`, `DVI_ONLY`, `TEST_PATTERN`), but test pattern now runs margin fill + `mode_scale_fn` from a static 320px SRAM line every third active line. Expected display: centered vertical color bars with black side margins.
- 2026-04-24 01:45:59 -0300: User reported synthetic 3x scaler path looks clean. Strong hypothesis: 256-line ring can wrap while Core 1 still reads an old frame; because Core 0 only advances `write_idx` after line commit, reader can see old line as ready while writer is overwriting the same modulo slot. Next test: configurable `NEOPICO_LINE_RING_SIZE=512`, live 720p DVI-only, no test pattern.
- 2026-04-24 01:47:02 -0300: Added configurable `NEOPICO_LINE_RING_SIZE` (default 256) and built live 720p DVI-only with `NEOPICO_LINE_RING_SIZE=512`, `NEOPICO_VIDEO_TEST_PATTERN=OFF`. Size `text=39328 bss=447072`; `g_line_ring` size `0x50010`. Flashed successfully. Expected display: live game video, no audio/Data Islands, larger ring to prevent modulo overwrite race.
- 2026-04-24 01:48:21 -0300: User reported 512-line ring live DVI-only has the same level of pixel glitches. Ring capacity/wrap is unlikely primary cause. Remaining suspect: live Core 1 reads from `g_line_ring` under capture/bus conditions, likely SRAM bank contention or per-line source access timing.
- 2026-04-24 01:50:16 -0300: Added temporary `NEOPICO_CAPTURE_FREEZE_AFTER_FRAME` option. When enabled, `video_capture_run()` captures one complete MVS frame, commits it to `g_line_ring`, aborts capture DMA, disables MVS PIO SMs, and idles Core 0. This tests reading real `g_line_ring` data with no concurrent capture writes/bus pressure.
- 2026-04-24 01:51:05 -0300: Built and flashed freeze-after-one-frame 720p DVI-only test. Flags: `NEOPICO_VIDEO_720P=ON`, `NEOPICO_VIDEO_DVI_ONLY=ON`, `NEOPICO_VIDEO_TEST_PATTERN=OFF`, `NEOPICO_LINE_RING_SIZE=256`, `NEOPICO_CAPTURE_FREEZE_AFTER_FRAME=ON`. Expected display: frozen captured game frame, no audio/Data Islands.
- 2026-04-24 01:52:55 -0300: User reported freeze-after-one-frame captured image still shows same artifacts. This clears concurrent capture writes/bus pressure as primary cause. Artifacts are likely already present in the captured RGB565 frame data under 720p build conditions, or specific to reading/scaling captured ring data. Next focus: 372 MHz sysclk effect on MVS capture PIO/DMA/input sampling.
- 2026-04-24 01:53:47 -0300: Patched `video_capture.c` to derive sync/pixel SM clkdiv from `clock_get_hz(clk_sys) / 126MHz`, clamped to >=1.0. Goal: keep MVS capture PIO instruction timing near the known-good 126 MHz behavior even when 720p sets sysclk to 372 MHz; restores post-PCLK settle delay from ~5.4ns back toward ~15.9ns for the two PIO NOPs.
- 2026-04-24 01:54:42 -0300: Built and flashed freeze-after-one-frame 720p DVI-only test with capture PIO clkdiv fix. Build flags unchanged from prior freeze test. `pi flash` needed reboot retry once, then loaded UF2 and rebooted into app.
- 2026-04-24 01:56:12 -0300: User reported freeze-after-one-frame image looks clean with capture PIO clkdiv fix. Root cause confirmed: 720p sysclk=372MHz made capture PIO sample too soon after MVS PCLK. Next validation: continuous live capture with DVI-only/no-audio, freeze disabled.
- 2026-04-24 01:45:02 -0300: User reported synthetic 3x scaler test looks clean. This clears 720p HSTX basics, DVI mode, line buffer, margin fills, and 3x scaler. Remaining likely culprit: live MVS line-ring source path / concurrent Core 0 write vs Core 1 read / read-frame selection.
- 2026-04-24 01:57:46 -0300: User clarified the freeze-after-one-frame artifacts look clean now. Proceeding to flash continuous live 720p DVI-only/no-audio with capture PIO clkdiv fix and freeze disabled.
- 2026-04-24 01:58:10 -0300: Flashed continuous live 720p DVI-only/no-audio build (`NEOPICO_VIDEO_720P=ON`, `DVI_ONLY=ON`, `TEST_PATTERN=OFF`, `FREEZE=OFF`) with capture PIO clkdiv fix. `pi flash` rebooted to BOOTSEL and loaded UF2 successfully.
2026-04-26 10:03:13 -03 - User reported OSD build fails because a5e032d 240p-mode branches doubled video_pipeline_scanline_callback SCRATCH_X use (~452 -> ~932 bytes), causing .stack1_dummy/.scratch_y overlap; need methodical read-only diagnosis before any code change.
2026-04-26 10:09:30 -03 - Fixed OSD SCRATCH_X overflow by compile-time specializing video_pipeline scanline mapping/scaler selection for default/240p/720p and moving RGB565 fill out of the scratch-X callback as a noinline/noclone scratch-Y helper. Verified builds: build_osd, build, build_commit_240p, build_720p, build_720p_nonrt, build_verify_osd_240p, build_verify_osd_720p. New OSD scratch_x sizes: default 0x798, OSD+240p 0x788, OSD+720p 0x778; all below 0x800 stack boundary.
2026-04-26 10:12:20 -03 - User asked how to flash the OSD build. Current OSD UF2 path from verified build is build_osd/src/neopico_hd.uf2; answer should prefer explicit build + flash commands and not flash without confirmation.
2026-04-26 10:16:42 -03 - User requested build and flash of 720p OSD firmware. Plan: configure build_osd_720p with NEOPICO_ENABLE_OSD=ON and NEOPICO_VIDEO_720P=ON, build neopico_hd, then flash build_osd_720p/src/neopico_hd.uf2 via pi flash.
2026-04-26 10:17:17 -03 - Built and flashed 720p OSD firmware from build_osd_720p/src/neopico_hd.uf2 (NEOPICO_ENABLE_OSD=ON, NEOPICO_VIDEO_720P=ON). pi flash rebooted RP2350 to BOOTSEL, loaded UF2, and rebooted to application mode. ELF size text=43512 data=0 bss=367740; scratch_x=0x778, scratch_y=0xd4.
2026-04-26 10:18:34 -03 - User asked how the SCRATCH_X overflow was fixed. Explanation: compile-time mode specialization removed runtime 240p/720p branches from the hot scanline callback; fill helper moved out of scratch_x into noinline/noclone scratch_y code; no Core 1 stack-size change.
2026-04-26 10:20:14 -03 - User asked whether compile-time specialization means runtime resolution switching is impossible. Answer: not impossible, but current fixed build no longer supports cross-resolution runtime switching through one generic callback; a future runtime-switch design should swap small mode-specific callbacks or gate the generic path behind an experiment, with scratch_x budget checked.
2026-04-26 10:22:08 -03 - User committed the OSD scratch fix and asked whether we can safely investigate runtime resolution switching. Safe path: read-only sizing/API inspection first; if implemented, put runtime switch experiment behind a compile-time flag, build-only validate scratch budgets, and do not flash unless explicitly requested.
2026-04-26 10:25:52 -03 - User asked whether PicoHDMI rt path is better suited for runtime resolution switching and whether to adopt it as default. Answer should note current normal default is already rt unless NEOPICO_USE_NONRT_HDMI=ON; rt is better for 480p/240p switching, but 720p default needs cautious validation because non-rt has been release-preferred for 720p stability and some 720p details remain compile-time sensitive.
2026-04-26 10:27:13 -03 - User approved starting runtime resolution switching work. Scope for first experiment: flag-gated 480p<->240p runtime switch only, no 720p, build/map validation first, no flashing unless explicitly requested.
2026-04-26 10:33:18 -03 - Implemented build-only runtime 480p<->240p switch experiment behind NEOPICO_EXP_RUNTIME_MODE_SWITCH=ON, OSD-only via Back button, rt-only, explicitly incompatible with 720p/nonrt. Verified default, OSD, 720p, 240p, runtime-switch, and runtime-switch-starting-240p builds. Runtime switch scratch_x=0x710, scratch_y=0xd4; not flashed.
2026-04-26 10:50:12 -03 - User asked to flash runtime-switch experiment. Rebuilt build_runtime_switch successfully and flashed build_runtime_switch/src/neopico_hd.uf2 via pi flash; tool rebooted RP2350 to BOOTSEL, loaded UF2, and rebooted into application mode. Expected test: OSD Back button toggles requested 480p/240p runtime mode.
2026-04-26 10:53:34 -03 - User reported runtime 480p/240p switch never recovers after switching. New direction proposed: persist desired output mode in flash and soft-reset so boot selects the mode cleanly instead of live HSTX retiming. Caveat from docs/KNOWN_ISSUES.md: soft reset itself can sometimes leave HDMI sink in No Signal, so this needs a cautious experiment.
2026-04-26 11:01:32 -03 - Read-only check for "fresh start" reset path. RP2350 docs expose RESET_HSTX bit 3 and SDK RESET_HSTX/reset_unreset_block_num_wait_blocking(); HSTX CSR.EN low only resets shift counter/clock generator and stops FIFO popping. Current rt hstx_resync uses CSR.EN, not subsystem RESET_HSTX. Next low-risk experiment should be HSTX-block reset/reprogram on mode switch before trying flash-persist reboot.
2026-04-26 11:07:46 -03 - Implemented and flashed HSTX-block-reset runtime-switch experiment. pico_hdmi rt mode-switch path now uses RESET_HSTX + HSTX/DMA reprogramming when NEOPICO_EXP_RUNTIME_MODE_SWITCH=ON; default path keeps normal resync. Verified builds: build, build_osd, build_720p, build_720p_nonrt, build_runtime_switch, build_runtime_switch_240p_start. Runtime switch scratch_x=0x668, scratch_y=0xd4. Flashed build_runtime_switch/src/neopico_hd.uf2 successfully.
2026-04-26 11:14:08 -03 - User reported HSTX-block-reset runtime switch still does not recover. This rules out stale HSTX peripheral state as the sole cause. Next experiment: whole-chip watchdog reboot using watchdog scratch registers to carry requested 480p/240p mode, before adding persistent flash settings.
2026-04-26 11:17:14 -03 - Implemented and flashed watchdog-reboot switch experiment. OSD Back now stores requested 480p/240p in watchdog scratch[0..2] with magic/check, calls watchdog_reboot(0,0,10), and main consumes/clears the request before HDMI init. Verified build, build_osd, build_720p, build_720p_nonrt, build_runtime_switch, build_runtime_switch_240p_start; runtime scratch_x=0x668. Flashed build_runtime_switch/src/neopico_hd.uf2 successfully.
2026-04-26 11:18:37 -03 - User reported watchdog-reboot 480p/240p switching worked. Conclusion: 240p/480p boot-time mode selection and scanline geometry are viable; failure is specific to live HDMI retiming/recovery. Next cleanup should make this an intentional reboot-required mode switch, remove/disable failed live-set_mode path for UI, and consider flash persistence separately.
2026-04-26 11:28:51 -03 - Cleaned experiment into explicit NEOPICO_EXP_REBOOT_MODE_SWITCH. Removed failed pico_hdmi HSTX-block-reset edits from working tree; only main firmware changed. Added deprecated CMake alias for NEOPICO_EXP_RUNTIME_MODE_SWITCH to avoid stale build-dir breakage. Fresh builds passed: build_reboot_switch, build_reboot_switch_240p_start, build, build_osd, build_720p, build_720p_nonrt; old build_runtime_switch also builds with warning. Reboot switch scratch_x=0x710, scratch_y=0xd4. Flashed build_reboot_switch/src/neopico_hd.uf2 successfully.
2026-04-26 11:49:09 -03 - User asked whether 720p is next. Key constraint: 720p mode still depends on compile-time PicoHDMI sync polarity and Data Island placement (VIDEO_MODE_1280x720 / MODE_SYNC_POSITIVE / DI_IN_HSYNC), plus 372 MHz sysclk and 1.30V VREG. Extending reboot switch to include 720p requires making those PicoHDMI mode attributes runtime first; otherwise one firmware image cannot safely cover 480p/240p and 720p.
2026-04-26 11:52:58 -03 - User approved "your call" on 720p next steps. Decision: keep working 480p/240p reboot switch intact and treat 720p as a separate flag-gated experiment. First step is read-only mapping of PicoHDMI rt compile-time mode assumptions, audio DI timing, and boot clock/vreg ordering before any code changes.
2026-04-26 12:10:53 -03 - Implemented NEOPICO_EXP_REBOOT_MODE_SWITCH_720P experiment. PicoHDMI rt can use runtime sync polarity/DI placement behind PICO_HDMI_RT_RUNTIME_MODE_ATTRS; watchdog scratch mode is now 480p/240p/720p; OSD Back cycles 480p -> 240p -> 720p -> 480p only when the 720p flag is ON. Initial 720p-switch build overflowed SCRATCH_X by 488 bytes; fixed by preventing build_line_with_di scratch clone and moving hstx_resync out of scratch_x. Verified builds: build, build_osd, build_720p, build_720p_nonrt, build_osd_720p, build_reboot_switch, build_reboot_switch_240p_start, build_reboot_switch_720p, build_runtime_switch. 720p-switch scratch_x=0x788, scratch_y=0xd4.
2026-04-26 12:11:28 -03 - Flashed experimental 720p reboot-switch firmware from build_reboot_switch_720p/src/neopico_hd.uf2. pi flash rebooted RP2350 to BOOTSEL, loaded UF2, and rebooted to application mode. Expected OSD Back cycle: 480p -> 240p -> 720p -> 480p, each via watchdog reboot.
2026-04-26 12:25:24 -03 - User requested flashing the experimental 720p reboot-switch firmware again. Flashed build_reboot_switch_720p/src/neopico_hd.uf2 successfully; pi flash rebooted RP2350 to BOOTSEL, loaded UF2, and rebooted to application mode.
2026-04-26 12:42:35 -03 - User reported the experimental 720p reboot-switch firmware "looks pretty good" on hardware. Treat current 480p/240p/720p watchdog-reboot mode switch as promising; next recommended step is commit only after user explicitly asks, noting the change includes nested lib/pico_hdmi modifications.
2026-04-26 12:45:05 -03 - User asked to port the PicoHDMI changes back to ~/Projects/pico_dmi. That path does not exist locally; ~/Projects/pico_hdmi exists and matches the submodule name, so use it as the likely intended upstream target after read-only status inspection.
2026-04-26 12:50:49 -03 - Ported PicoHDMI runtime sync polarity/Data Island changes to standalone ~/Projects/pico_hdmi. Copied six modified library files from lib/pico_hdmi and added a standalone CMake option PICO_HDMI_RT_RUNTIME_MODE_ATTRS to enable the guarded path cleanly. Standalone repo validation passed: bouncing_box default, bouncing_box 720p non-RT, bouncing_box_rt, bouncing_box_rt with PICO_HDMI_RT_RUNTIME_MODE_ATTRS=ON, directvideo_240p. Target repo still has unrelated untracked ANGETS.md.
2026-04-26 15:43:32 -03 - User requested committing/pushing standalone pico_hdmi and updating the lib/pico_hdmi reference here. Standalone pico_hdmi is on main at origin/main 2779ce9 before commit, with intended tracked changes plus unrelated untracked ANGETS.md left unstaged.
2026-04-26 15:48:41 -03 - Standalone ~/Projects/pico_hdmi commit e15b16d31464bd61fbb2e86f2e56208fcb716b12 ("Add runtime HDMI mode attributes") was created and pushed to origin/main. Pre-commit passed, and PicoHDMI example verification builds passed for rt, rt runtime-attrs option, 720p non-rt, and directvideo_240p. Unrelated untracked ANGETS.md remains untouched.
2026-04-26 15:50:52 -03 - Updated neopico-hd lib/pico_hdmi submodule working tree reference from 2779ce9 to pushed PicoHDMI commit e15b16d. Submodule is clean internally. NeoPico verification passed for build, build_osd, build_720p, build_720p_nonrt, build_reboot_switch, and build_reboot_switch_720p; 720p reboot-switch scratch remains .scratch_x=1928 and .scratch_y=212.
2026-04-26 15:53:58 -03 - User requested committing neopico-hd. Commit scope is the watchdog-reboot resolution switch firmware changes, PicoHDMI submodule reference e15b16d, and scratchbook history; no internal submodule dirt.
2026-04-26 16:04:28 -03 - Added flag-gated temperature telemetry path (`NEOPICO_EXP_TEMP_TELEMETRY`) using RP2350 ADC temp sensor read (`adc_set_temp_sensor_enabled`, `ADC_TEMPERATURE_CHANNEL_NUM`) with 2s USB stdio logging from background task. Built and flashed `build_temp_telemetry`; observed runtime readings around 41.18C to 47.27C (`TEMP:` lines on `/dev/cu.usbmodem1301`). Change is experimental and currently uncommitted (`src/main.c`, `src/CMakeLists.txt`).
2026-04-26 16:05:41 -03 - Rebuilt and flashed 720p runtime-path firmware from `build_720p/src/neopico_hd.uf2` (`NEOPICO_VIDEO_720P=ON`, `NEOPICO_USE_NONRT_HDMI=OFF`). Flash succeeded via pi helper with BOOTSEL reboot and return to application mode.
2026-04-26 16:06:32 -03 - User needs live temperature readings while running 720p RT. Current flashed 720p RT build has `NEOPICO_EXP_TEMP_TELEMETRY` off, so build a separate 720p RT + temp telemetry UF2 and read USB serial `TEMP:` output after flashing.
2026-04-26 16:07:47 -03 - Built and flashed `build_720p_temp_telemetry` with `NEOPICO_VIDEO_720P=ON`, `NEOPICO_USE_NONRT_HDMI=OFF`, and `NEOPICO_EXP_TEMP_TELEMETRY=ON`. USB serial on `/dev/cu.usbmodem1301` reported 720p runtime temps: 48.20C, 50.08C, 45.86C, 46.80C, 48.67C. Board is currently running the 720p RT temp-telemetry firmware.
2026-04-26 16:13:45 -03 - User serial log showed impossible single-sample temp outliers (e.g. 55.23C -> 32.29C -> 23.39C -> 8.41C -> 51.95C). Updated temp telemetry to discard one mux-settling read, collect 11 ADC samples, sort, and report median plus raw min/max/spread. Rebuilt/flashed `build_720p_temp_telemetry`; median readings were 51.48C, 49.14C, 49.61C, 49.14C, 47.27C, 50.08C with raw spread showing occasional ADC/reference noise.
2026-04-26 16:18:41 -03 - User noted 720p RT temperature readings around 47-51C are with a small heatsink. Assessment: comfortable thermal margin for RP2350 at 372MHz/1.30V, but ADC temp is uncalibrated and reference-sensitive; use as trend/soak-test signal rather than absolute lab measurement.
2026-04-26 16:25:23 -03 - Removed temp telemetry changes from active source/build flags because ADC sampling disrupted the signal. Added OSD resolution selector for reboot-switch builds: Menu opens selector, Back cycles target resolution, Menu applies; selecting the current mode closes OSD, selecting another mode writes watchdog scratch and reboots. Flashed `build_reboot_switch_720p/src/neopico_hd.uf2`; default, OSD, and reboot-switch-720p builds passed. Working tree now only has selector change plus scratchbook.
2026-04-26 18:53:31 -03 - User reported reboot-switch firmware did not crash at 720p, but 480p loses sync much faster and visibly jiggles vertically. This points away from 720p heat/load and toward 480p output/capture cadence mismatch or 480p HDMI/audio timing behavior. Plain `build_720p` cache is vanilla 720p RT (`720p=ON`, `nonrt=OFF`, OSD/switch off), but hold off flashing until deciding the better isolation test.
2026-04-26 18:54:11 -03 - User clarified the firmware was not crashing because the OSD menu stayed functional during the 480p failure. Interpretation: HSTX/Core1/OSD remain alive; likely failure is captured video/ring synchronization or HDMI-vs-MVS cadence presentation, not whole-firmware hang or HDMI output loss.
2026-04-26 18:55:24 -03 - Built and flashed cadence-isolation firmware `build_vtotal_match_532/src/neopico_hd.uf2`: RT 480p, `NEOPICO_EXP_VTOTAL_MATCH=ON`, `NEOPICO_EXP_VTOTAL_LINES=532`, OSD/reboot-switch/720p/240p all OFF. Expected output cadence ~25.2MHz/(800*532)=59.21Hz to reduce 480p vertical jiggle if MVS-vs-HDMI drift is the cause.
2026-04-26 18:58:29 -03 - User reported the 480p vtotal-match 532 isolation firmware seems to work fine. Conclusion: 480p instability is likely 60.00Hz HDMI vs ~59.18Hz MVS cadence drift/frame-phase issue, not firmware crash; next integration should make reboot-switch 480p use v_total=532 and likely 240p use v_total=266, leaving 720p standard.
2026-04-26 19:04:57 -03 - User requested returning to vanilla 720p RT to focus on overclock/voltage stability. Rebuilt and flashed `build_720p/src/neopico_hd.uf2`; cache confirms `NEOPICO_VIDEO_720P=ON`, `NEOPICO_USE_NONRT_HDMI=OFF`, OSD/reboot-switch/vtotal-match all OFF. This firmware uses 372MHz sysclk and 1.30V VREG via the 720p boot path.
2026-04-26 19:58:12 -03 - User reported vanilla 720p RT has been running strong for about an hour. Treat 372MHz/1.30V 720p RT overclock/voltage as provisionally stable on this board/heatsink/environment; remaining instability is more likely OSD/selector, live capture presentation, or cadence interactions than raw 720p clock/VREG.
2026-04-26 19:59:39 -03 - Built and flashed 720p RT + OSD-only isolation firmware `build_720p_osd_isolation/src/neopico_hd.uf2`: `NEOPICO_VIDEO_720P=ON`, `NEOPICO_USE_NONRT_HDMI=OFF`, `NEOPICO_ENABLE_OSD=ON`, reboot-switch/vtotal-match OFF. Scratch usage .scratch_x=1704, .scratch_y=212. Purpose: isolate OSD overlay path at 372MHz/1.30V without selector/reboot switching.
2026-04-26 20:04:26 -03 - User reported 720p RT + OSD-only isolation looks stable. Conclusion: OSD overlay itself is not the instability trigger at 720p/372MHz. Remaining suspects are selector/reboot-switch integration and/or 480p cadence/presentation path.
2026-04-26 20:07:17 -03 - Integrated known-good 480p cadence into selector/reboot-switch firmware: reboot-switch 480p now copies `video_mode_480_p` and changes `v_total_lines` to 532 with adjusted back porch before calling `video_output_set_mode`. 720p remains standard; 240p unchanged. Rebuilt and flashed `build_reboot_switch_720p/src/neopico_hd.uf2`; scratch remains .scratch_x=1928, .scratch_y=212.
2026-04-26 20:09:22 -03 - User reported selector/reboot-switch firmware with 480p v_total=532 still jiggles until it loses sync. This means cadence match alone is insufficient once the selector/OSD-enabled firmware path is present. Next isolation: 480p RT + v_total=532 + OSD ON, with reboot-switch/selector OFF.
2026-04-26 20:10:42 -03 - Built and flashed 480p RT + OSD-only + v_total=532 isolation firmware from `build_480p_osd_vtotal532/src/neopico_hd.uf2`: `NEOPICO_ENABLE_OSD=ON`, `NEOPICO_EXP_REBOOT_MODE_SWITCH=OFF`, `NEOPICO_EXP_VTOTAL_MATCH=ON`, `NEOPICO_EXP_VTOTAL_LINES=532`, `NEOPICO_USE_NONRT_HDMI=OFF`, 720p/240p OFF. Scratch usage .scratch_x=1688 and .scratch_y=212. Purpose: test whether 480p OSD overlay plus cadence match is stable without selector/reboot-switch code.
2026-04-26 20:14:40 -03 - User reported the 480p RT + OSD-only + v_total=532 isolation build is stable, and questioned whether v_total=532 is necessary. Current interpretation: this proves OSD overlay alone is not enough to reproduce the selector firmware failure under this cadence, but does not prove the v_total tweak is required; next clean isolation is 480p RT + OSD-only with default v_total=525 and reboot-switch OFF.
2026-04-26 20:15:57 -03 - Recommended next split: build/flash 480p RT + OSD-only with default 525-line timing and reboot-switch OFF. If stable, drop the 532-line hypothesis and focus on selector/reboot-switch integration; if unstable, cadence matching is likely relevant specifically in the 480p OSD path.
2026-04-26 20:23:52 -03 - User reported stability again after the 480p RT + OSD-only + v_total=532 isolation. Treat this as confirmation that OSD overlay is stable in 480p when selector/reboot-switch code is absent under the tested timing. Next still needed: same OSD-only isolation with default 525-line timing.
2026-04-26 20:24:41 -03 - Built and flashed 480p RT + OSD-only default-timing isolation firmware from `build_480p_osd_default/src/neopico_hd.uf2`: `NEOPICO_ENABLE_OSD=ON`, `NEOPICO_EXP_REBOOT_MODE_SWITCH=OFF`, `NEOPICO_EXP_VTOTAL_MATCH=OFF`, `NEOPICO_USE_NONRT_HDMI=OFF`, 720p/240p OFF. Cache still has `NEOPICO_EXP_VTOTAL_LINES=532` string but it is inactive because vtotal-match is OFF. Scratch usage .scratch_x=1688 and .scratch_y=212.
2026-04-26 20:28:14 -03 - User reported the default-timing 480p RT + OSD-only isolation is stable. User suspects the long-running OSD issue is not the overlay itself, but what content is rendered and how the OSD buffer is updated. Code inspection supports this direction: selector path does full `fast_osd_clear()` plus many cell renders into the same `osd_framebuffer` the scanline callback reads; current OSD-only isolation renders no menu content.
2026-04-26 20:36:49 -03 - User clarified OSD constraints: do not call `fast_osd_clear()` during updates because it is too expensive; use the glyph-level update path so only necessary glyphs change; avoid realtime computation by preprocessing/caching everything possible. Treat full live OSD redraws as unsafe unless done before scanout or while hidden.
2026-04-26 20:39:53 -03 - Refactored selector OSD update path: full selector render happens before `osd_show()` on open; Back now updates only the previous and newly selected option rows via glyph-level `fast_osd_put*` calls; apply hides OSD before watchdog reboot instead of clearing/rendering a transient switching page. Removed the temporary 480p reboot-switch `v_total=532` tweak by returning to `video_mode_480_p`. Rebuilt and flashed `build_reboot_switch_720p/src/neopico_hd.uf2`; config is OSD ON, reboot-switch ON, 720p selectable, RT path, vtotal-match OFF; scratch .scratch_x=1928, .scratch_y=212.
2026-04-26 20:45:00 -03 - User reported revised selector firmware still drops sync shortly at 480p, but OSD is notably more stable. Interpretation: dirty-row update reduced one failure mode, but remaining issue likely comes from another difference between stable 480p OSD-only and selector firmware. Stable OSD-only build has .scratch_x=1688; selector/reboot-switch build has .scratch_x=1928 and uses the multi-mode runtime scanline callback path.
2026-04-26 20:50:23 -03 - Built a two-mode selector isolation (`build_reboot_switch_no720p`): OSD ON, reboot-switch ON, 720p selection OFF, RT path, vtotal-match OFF. Scratch usage .scratch_x=1680 and .scratch_y=212, much closer to stable OSD-only. Attempted to flash via `pi flash` and direct `picotool load -f -x`, but current board would not enter accessible BOOTSEL; commands reported a USB serial device at bus 0/address 3 and tracked a bogus serial. No confirmed flash progress occurred, so do not assume this UF2 is running.
2026-04-26 20:53:44 -03 - User reported "same issue" after the two-mode selector isolation. Interpretation: disabling 720p-selectable support is not enough; remaining suspect is the `NEOPICO_EXP_REBOOT_MODE_SWITCH` dynamic scanline callback path vs the stable static 480p OSD callback shape. Next change: select a mode-specific scanline callback at boot so 480p reboot-switch uses the static 480p implementation.
2026-04-26 20:56:49 -03 - Added dedicated static 480p scanline callback for reboot-switch builds and register it only when booted mode is 480p; 240p/720p remain on the dynamic callback for now. Trimmed the dynamic callback so no-720p builds remove 720p checks/scaling code. Rebuilt and flashed `build_reboot_switch_no720p/src/neopico_hd.uf2`; confirmed real UF2 progress. Config: OSD ON, reboot-switch ON, 720p selection OFF, RT path, vtotal-match OFF. Scratch .scratch_x=1992, .scratch_y=212.
2026-04-26 21:01:31 -03 - User reported the static-480p two-mode selector still drops sync, though it took longer, and it drops even without opening OSD. User has seen this before: binary firmware layout alone can disturb stability. Interpretation: stop treating live OSD rendering as the primary trigger; focus on deterministic placement/timing of hot code and binary-layout sensitivity.
2026-04-26 21:09:35 -03 - Layout isolation build prepared in `build_reboot_switch_no720p`: 480p reboot-switch now registers a dedicated `.scratch_x.000_video_pipeline_480p` callback; non-480p selector fallback moved to `.scratch_y.100_reboot_modes`. Hot `.scratch_x` map now matches stable 480p OSD-only exactly: vsync `0x20080000`, 480p scanline `0x20080020` size 0x1e8, DMA IRQ `0x20080350`, `.scratch_x=1688`, stack1 bottom `0x20080800`. Flash attempt failed because the board was not visible as BOOTSEL or USB serial; no confirmed flash.
2026-04-26 21:10:45 -03 - Retried flash and successfully programmed `build_reboot_switch_no720p/src/neopico_hd.uf2`; confirmed real UF2 flash progress and reboot to application. This is the layout-matched 480p selector isolation build described in the previous entry.
2026-04-26 21:12:02 -03 - User reported "same" on the layout-matched `.scratch_x` build. This rules out `.scratch_x` placement alone. Remaining mismatch: stable OSD-only has `.scratch_y` helpers at fill=0x20081000, double=0x20081018, triple=0x20081054, quad=0x20081090; failing selector build has double=0x20081000, quad=0x2008103c, fill=0x20081080, reboot fallback=0x20081098. Next attempt: restore helper section layout exactly and move non-480p fallback out of scratch RAM.
2026-04-26 21:19:39 -03 - Prepared and flashed stricter 480p layout isolation. For no-720p reboot-switch build, non-480p scanline fallback is compiled out of the no-720p image, so this UF2 is only valid for idle 480p testing (do not switch to 240p). Hot layout now matches stable 480p OSD-only exactly at symbol level: `.scratch_x` vsync=0x20080000, 480p scanline=0x20080020 size 0x1e8, DMA IRQ=0x20080350, end=0x20080698; `.scratch_y` fill=0x20081000, double=0x20081018, triple=0x20081054, quad=0x20081090, end=0x200810d4. Flash confirmed by UF2 progress.
2026-04-26 21:22:06 -03 - User reported strict layout-isolation selector build still dropped sync. Sanity check: flashed known-stable 480p RT + OSD-only default-timing build from `build_480p_osd_default/src/neopico_hd.uf2` (`NEOPICO_ENABLE_OSD=ON`, reboot-switch OFF, vtotal-match OFF, RT path, 720p/240p OFF; artifact built Apr 26 20:24). Purpose: verify board/input/environment still matches prior stable baseline before continuing selector debugging.
2026-04-26 21:26:15 -03 - User reported the 480p RT + OSD-only default-timing sanity build is stable even with the OSD opened. This reconfirms the board/input/environment and visible OSD overlay path are stable. Remaining suspect is selector/reboot-switch side effects, especially background tick/polling/time calls or XIP/code-layout effects outside the hot scratch callback/helper placement.
2026-04-26 21:28:15 -03 - Began selector background isolation. Changed reboot-switch/non-selftest background tick so when OSD is hidden it polls only the MENU button, opens the selector on a MENU edge, resets Back state, and returns before `to_ms_since_boot`, Back GPIO polling, debounce updates, or selector work run continuously. Target test build remains `build_reboot_switch_no720p` (OSD ON, reboot-switch ON, 720p selectable OFF, RT path, default timing).
2026-04-26 21:28:59 -03 - Built and flashed `build_reboot_switch_no720p/src/neopico_hd.uf2` with hidden-idle selector background isolation. Flash confirmed by UF2 progress and reboot. Build flags: OSD ON, reboot-switch ON, 720p selectable OFF, RT path, default timing; scratch `.scratch_x=1688`, `.scratch_y=212`. Current video path remains strict 480p layout isolation, so this image is for idle 480p testing only; do not switch modes in this build.
2026-04-26 21:30:21 -03 - User reported hidden-idle selector isolation still drops sync. This rules out continuous hidden Back polling, debounce, and `to_ms_since_boot` work as primary triggers. Scratch helper `.scratch_y` bytes match stable exactly; `.scratch_x` code shape/addresses match but literals differ because BSS addresses shifted (`osd_visible`/latched moved by +4, `rt_v_total_lines` moved by +4). Next isolation: avoid the reboot-switch boot path calling `video_output_set_mode(&video_mode_480_p)` when booting the default 480p mode, matching stable init behavior more closely.
2026-04-26 21:32:07 -03 - Changed 480p reboot-switch boot to skip `video_output_set_mode(video_output_mode_for_reboot_mode(...))` when `reboot_boot_mode` is already `VIDEO_PIPELINE_REBOOT_MODE_480P`, so default 480p init follows the same no-pending-mode path as stable OSD-only. Built and flashed `build_reboot_switch_no720p/src/neopico_hd.uf2`; scratch remains `.scratch_x=1688`, `.scratch_y=212`, hot symbols unchanged. Flash confirmed by UF2 progress and reboot.
2026-04-26 21:32:53 -03 - User reported the 480p default-init reboot-switch isolation still drops sync. This rules out the pre-init `video_output_set_mode(&video_mode_480_p)` path. Current result set: stable OSD-only is good even with OSD visible; all reboot-switch/selector variants fail at 480p even after hidden background work was minimized and hot scratch placement was matched. Next recommended isolation: invert the experiment by adding the smallest possible reboot-switch footprint to the stable OSD-only path, with selector/runtime mode code disabled, to see whether the compile-time reboot-switch path itself is destabilizing.
2026-04-26 21:35:51 -03 - Added off-by-default `NEOPICO_EXP_DISABLE_BACKGROUND_TASK` experiment flag. Built and flashed `build_reboot_switch_no720p_nobg/src/neopico_hd.uf2` with OSD ON, reboot-switch ON, 720p selectable OFF, RT path, default timing, and Core 1 background task disabled. This removes `combined_background_task()` entirely for the test, so there is no audio background task and no OSD button/menu polling. Scratch remains `.scratch_x=1688`, `.scratch_y=212`; flash confirmed by UF2 progress and reboot.
2026-04-26 21:45:21 -03 - User reported the no-background reboot-switch isolation is stable. This proves the reboot-switch binary/hot scratch layout can be stable at idle 480p, and points to something executed through the Core 1 background hook as the immediate trigger. Next split: re-enable only audio background vs only OSD/menu background to identify which branch destabilizes this binary.
2026-04-26 21:47:03 -03 - Added off-by-default branch-isolation flags `NEOPICO_EXP_DISABLE_AUDIO_BACKGROUND` and `NEOPICO_EXP_DISABLE_OSD_BACKGROUND`. Built and flashed audio-only background isolation from `build_reboot_switch_no720p_audioonly/src/neopico_hd.uf2`: OSD ON, reboot-switch ON, 720p selectable OFF, Core 1 background hook enabled, audio branch enabled, OSD/menu branch disabled. Scratch remains `.scratch_x=1688`, `.scratch_y=212`; flash confirmed by UF2 progress and reboot.
2026-04-26 21:48:28 -03 - User reported audio-only background isolation dropped sync. This implicates the audio branch of the Core 1 background hook in the reboot-switch binary. Inspection: audio starts after 120 frames, initializes PIO2 and an I2S DMA ring (`dma_claim_unused_channel`, 16 KB circular buffer), then SRC/encodes/pushes HDMI audio packets. Added off-by-default `NEOPICO_EXP_AUDIO_NO_CAPTURE_START` to initialize audio but skip starting I2S capture DMA/PIO and skip audio processing, to split active capture DMA/PIO from the state-machine/background hook.
2026-04-26 21:50:50 -03 - Built and flashed no-capture audio isolation from `build_reboot_switch_no720p_audio_nocapture/src/neopico_hd.uf2`: OSD ON, reboot-switch ON, 720p selectable OFF, Core 1 background hook enabled, audio branch enabled, OSD/menu branch disabled, `NEOPICO_EXP_AUDIO_NO_CAPTURE_START=ON`. Audio init path is present, but I2S capture DMA/PIO start and SRC/output processing are skipped. Scratch remains `.scratch_x=1688`, `.scratch_y=212`; flash confirmed by UF2 progress and reboot.
2026-04-26 21:55:10 -03 - User reported the no-capture audio isolation is stable, then asked "I2S?". Interpretation: audio init/config alone is stable, including PIO2 program load, GPIO setup, DMA channel claim/config, and background hook presence. The instability is likely triggered after starting active I2S capture (PIO SM + DMA stream) or by the subsequent drain/SRC/HDMI audio packet path. Next split should start I2S capture but skip audio processing/output to isolate active PIO/DMA from CPU/DI queue work.
2026-04-26 22:03:41 -03 - Added off-by-default `NEOPICO_EXP_AUDIO_CAPTURE_ONLY`: starts I2S capture DMA/PIO normally but returns from `audio_background_task()` before ring drain, SRC, and HDMI audio packet enqueue. Built and flashed `build_reboot_switch_no720p_audio_captureonly/src/neopico_hd.uf2`: OSD ON, reboot-switch ON, 720p selectable OFF, audio branch enabled, OSD/menu branch disabled, `AUDIO_NO_CAPTURE_START=OFF`, `AUDIO_CAPTURE_ONLY=ON`. Scratch remains `.scratch_x=1688`, `.scratch_y=212`; flash confirmed by UF2 progress and reboot.
2026-04-26 22:06:45 -03 - User reported capture-only I2S isolation is stable. This rules out active I2S PIO/DMA traffic by itself. Remaining trigger is CPU-side audio work after capture starts: ring drain, SRC/drop processing, HDMI audio packet encode, and/or `hstx_di_queue_push`. Next split should process/drain captured audio but suppress HDMI packet enqueue to separate CPU processing load from DI queue mutation.
2026-04-26 22:08:20 -03 - Added off-by-default `NEOPICO_EXP_AUDIO_PROCESS_NO_OUTPUT`: runs capture and `audio_pipeline_process()` but returns from `audio_output_callback()` before HDMI audio packet encode or DI queue push. Built and flashed `build_reboot_switch_no720p_audio_process_noout/src/neopico_hd.uf2`: OSD ON, reboot-switch ON, 720p selectable OFF, audio branch enabled, OSD/menu branch disabled, `AUDIO_PROCESS_NO_OUTPUT=ON`. Scratch remains `.scratch_x=1688`, `.scratch_y=212`; flash confirmed by UF2 progress and reboot.
2026-04-26 22:09:14 -03 - User reported process-no-output audio isolation is stable. This rules out I2S capture, DMA, ring drain, and SRC/drop CPU work as primary trigger. Remaining trigger is HDMI audio output path in `audio_output_callback`: packet construction (`hstx_packet_set_audio_samples`), Data Island encode (`hstx_encode_data_island`), and/or `hstx_di_queue_push`/DI ring mutation consumed by the DMA ISR. Next split: encode packets but do not push to DI queue.
2026-04-26 22:10:50 -03 - Added off-by-default `NEOPICO_EXP_AUDIO_ENCODE_NO_QUEUE`: audio callback builds audio packets and encodes Data Islands but skips `hstx_di_queue_push`, still advancing local audio frame/collect state. Built and flashed `build_reboot_switch_no720p_audio_encode_noqueue/src/neopico_hd.uf2`: OSD ON, reboot-switch ON, 720p selectable OFF, audio branch enabled, OSD/menu branch disabled, `AUDIO_ENCODE_NO_QUEUE=ON`; final symbol list has no `hstx_di_queue_push` reference. Scratch remains `.scratch_x=1688`, `.scratch_y=212`; flash confirmed by UF2 progress and reboot.
2026-04-26 22:25:21 -03 - User reported encode-no-queue audio isolation took much longer but eventually dropped sync. Interpretation: DI queue mutation is not required; packet/Data-Island encode CPU cost is enough to cross the Core 1 timing margin over time, while queue push likely worsens it. The immediate unsafe pattern is the unbounded `while (ap_ring_available(...) > 0)` drain loop in Core 1 background, which can spend too long in audio work. Next practical fix: fixed per-pass audio work budget, likely one `audio_pipeline_process()` call per background pass.
2026-04-26 22:27:02 -03 - Added off-by-default `NEOPICO_EXP_AUDIO_LIMIT_BACKGROUND_WORK`: when enabled, `audio_background_task()` runs only one `audio_pipeline_process()` per Core 1 background pass and skips the unbounded drain-until-empty loop. Built and flashed `build_reboot_switch_no720p_audio_budget/src/neopico_hd.uf2`: full audio output path enabled (I2S capture, process, packet encode, DI queue push), OSD/menu background disabled, reboot-switch ON, 720p selectable OFF, `AUDIO_LIMIT_BACKGROUND_WORK=ON`. Scratch remains `.scratch_x=1688`, `.scratch_y=212`; flash confirmed by UF2 progress and reboot.
2026-04-26 22:28:33 -03 - User reported budgeted full-audio build dropped quickly. The one-process-call budget is insufficient because a single `audio_pipeline_process()` can still emit a batch of processed samples and `audio_output_callback()` can encode/push many HDMI audio packets in one Core 1 loop pass. Next isolation/fix: cap the output callback itself to one HDMI audio packet per background pass.
2026-04-26 22:30:56 -03 - Added off-by-default `NEOPICO_EXP_AUDIO_LIMIT_OUTPUT_PACKETS`: caps `audio_output_callback()` to one encoded/pushed HDMI audio packet per background pass. Built and flashed `build_reboot_switch_no720p_audio_packet_budget/src/neopico_hd.uf2` with full audio output path enabled, OSD/menu background disabled, `AUDIO_LIMIT_BACKGROUND_WORK=ON`, and `AUDIO_LIMIT_OUTPUT_PACKETS=ON`. This is a timing isolation that may drop extra processed audio samples; not an audio-quality candidate yet. Scratch remains `.scratch_x=1688`, `.scratch_y=212`; flash confirmed by UF2 progress and reboot.
2026-04-26 22:44:05 -03 - User reported packet-budget full-audio build still dropped. Since process-no-output is stable and encode-no-queue only failed much later, fast failure with one queued packet/pass points at the live DI queue path and/or dynamic audio packet contents, not just burst size. Next split: push a pre-encoded static silence Data Island through the DI queue so the ISR consumes queued packets, but dynamic packet generation/content is removed.
2026-04-26 22:47:05 -03 - Built and flashed static-silence queued audio isolation from `build_reboot_switch_no720p_audio_static_queue/src/neopico_hd.uf2`: full capture/process path enabled, OSD/menu background disabled, `AUDIO_LIMIT_BACKGROUND_WORK=ON`, `AUDIO_LIMIT_OUTPUT_PACKETS=ON`, `AUDIO_PUSH_STATIC_SILENCE=ON`. This pushes a pre-encoded static silence Data Island through `hstx_di_queue_push`, removing dynamic packet generation/content while exercising the live queued-packet ISR path. Scratch `.scratch_x=1688`, `.scratch_y=212`; flash confirmed by UF2 progress and reboot.
2026-04-26 22:47:41 -03 - User reported static-silence queued audio isolation still drops sync. This strongly implicates the live Data Island queue/consume path (`hstx_di_queue_push` plus DMA ISR queued-packet handling) rather than dynamic audio packet contents or SRC/ring processing. Next step: inspect HSTX DI queue/ISR implementation and test whether disabling queued Data Islands at 480p restores stability with the rest of audio running.
2026-04-26 22:50:32 -03 - Built and flashed A/B static-queue test from `build_480p_osd_default_audio_static_queue/src/neopico_hd.uf2`: reboot selector OFF, OSD ON, OSD/menu background disabled, `AUDIO_LIMIT_BACKGROUND_WORK=ON`, `AUDIO_LIMIT_OUTPUT_PACKETS=ON`, `AUDIO_PUSH_STATIC_SILENCE=ON`. This keeps the same queued static Data Island workload but removes the reboot-switch binary path. Scratch `.scratch_x=1688`, `.scratch_y=212`; hot HSTX ISR symbols match the stable default layout; flash confirmed by UF2 progress and reboot.
2026-04-26 22:53:04 -03 - User reported selector-off static queued Data Island build is stable. This rules out the live DI queue consume path as inherently unstable in the current OSD/audio firmware. The failure requires the reboot-switch/selector binary path plus queued DI activity, even when OSD/menu background is disabled and hot scratch ISR layout is unchanged. Next step: compare stable selector-off vs failing selector-on binaries for non-scratch code/data placement and memory-bank effects.
2026-04-26 22:56:49 -03 - Added flag-gated `NEOPICO_EXP_ALIGN_HSTX_DI_BUFFERS` experiment. When enabled it defines `PICO_HDMI_ALIGN_DI_BUFFERS=1` for pico_hdmi and 16-byte-aligns runtime HSTX command-list buffers in `video_output_rt.c`. Built and flashed `build_reboot_switch_no720p_audio_static_queue_aligned/src/neopico_hd.uf2`: selector ON, OSD/menu background disabled, static queued DI audio, command-list buffers now grouped/aligned at `0x20030210`..`0x20031370` instead of shifted late-BSS `0x2005c248`.., `.scratch_x=1696`, `.scratch_y=212`. Flash confirmed by UF2 progress and reboot.
2026-04-26 22:58:01 -03 - User reported aligned selector-on/static-queue build still drops sync. This rules out the simple +4 SRAM phase/unaligned command-list buffer hypothesis. Remaining suspect is selector-on code shape or different scratch contents interacting with queued DI timing; next compare exact `.scratch_x` disassembly/bytes between stable selector-off static-queue and failing selector-on static-queue builds.
2026-04-26 23:00:43 -03 - Added flag-gated `NEOPICO_EXP_RAM_DI_QUEUE_PUSH` experiment. When enabled it defines `PICO_HDMI_RAM_DI_QUEUE_PUSH=1` for pico_hdmi and places only `hstx_di_queue_push()` in SRAM via `__not_in_flash_func`. Built and flashed `build_reboot_switch_no720p_audio_static_queue_ram_push/src/neopico_hd.uf2`: selector ON, OSD/menu background disabled, static queued DI audio, alignment experiment OFF, `hstx_di_queue_push` at `0x20000ec8`, `.scratch_x=1688`, `.scratch_y=212`. Flash confirmed by UF2 progress and reboot.
2026-04-27 00:56:47 -03 - User reported RAM `hstx_di_queue_push()` selector-on/static-queue build still drops sync. This rules out XIP/code-placement of `hstx_di_queue_push` as the key trigger. Next split should separate the reboot-mode plumbing from the selector UI/rendering code compiled into `menu_diag_experiment.c`.
2026-04-27 00:58:52 -03 - Added flag-gated `NEOPICO_EXP_DISABLE_REBOOT_SELECTOR_UI` experiment. It keeps reboot-mode plumbing compiled but removes the OSD resolution selector helpers/data from `menu_diag_experiment.c`. Built and flashed `build_reboot_switch_no720p_audio_static_queue_no_selector_ui/src/neopico_hd.uf2`: selector plumbing ON, selector UI disabled, OSD/menu background disabled, static queued DI audio, RAM push/aligned DI buffers OFF. `.text=30392`, `.rodata=1600`, `.scratch_x=1688`, `.scratch_y=212`; no `resolution_selector_*` symbols; `reboot_requested_mode` still present. Flash confirmed by UF2 progress and reboot.
2026-04-27 11:33:44 -03 - User reported no-selector-UI reboot-plumbing/static-queue build is stable. Conclusion: reboot-mode plumbing alone is not the trigger; the failing condition requires selector UI code/data being compiled into the binary even when OSD/menu background is disabled. This points back to flash/code layout or XIP/cache sensitivity affecting executed audio/video code, not runtime selector behavior. Next test: keep selector UI compiled but isolate its placement so executed audio/HSTX paths return to a stable layout.
2026-04-27 11:36:45 -03 - Added `NEOPICO_EXP_RAM_SELECTOR_UI` experiment. When enabled, selector UI helpers and OSD render helpers (`fast_osd_clear`, `fast_osd_putc[_color]`, `fast_osd_puts[_color]`) are placed in SRAM. Built and flashed `build_reboot_switch_no720p_audio_static_queue_ram_selector_ui/src/neopico_hd.uf2`: selector UI enabled, OSD/menu background disabled, static queued DI audio, previous DI-align/RAM-push experiments OFF. Selector/render code now at `0x20000110`..`0x20000444`; audio/HSTX flash functions are within `0x20-0x24` of the stable no-selector build instead of shifted by `~0x340+`. `.text=30432`, `.rodata=2724`, `.data=5412`, `.scratch_x=1688`, `.scratch_y=212`. Flash confirmed by UF2 progress and reboot.
2026-04-27 12:16:33 -03 - User reported RAM-selector-UI/static-queue build is stable. This confirms the selector UI runtime behavior is not the trigger; the previous failure was caused by flash/code layout perturbation from cold selector/render code shifting timing-sensitive audio/HSTX code. Practical fix candidate: keep selector UI compiled but place cold selector/render helpers in SRAM, or alternatively place a broader hot audio/HSTX path in SRAM; the tested stable path is `NEOPICO_EXP_RAM_SELECTOR_UI=ON`.
2026-04-27 12:33:00 -03 - User approved proceeding to the production-like stability test. Plan: build/flash 480p default reboot selector with selector UI enabled, `NEOPICO_EXP_RAM_SELECTOR_UI=ON`, OSD/menu background enabled, normal dynamic audio path restored, no static queued DI isolation, no audio budget/isolation flags, and 720p selector disabled for this 480p/240p stability pass.
2026-04-27 12:35:14 -03 - Built and flashed production-like 480p/240p selector test from `build_reboot_switch_no720p_ram_selector_ui_prod/src/neopico_hd.uf2`: OSD ON, reboot selector ON, 720p selectable OFF, selector UI enabled, `NEOPICO_EXP_RAM_SELECTOR_UI=ON`, OSD background ON, normal dynamic audio ON, all audio isolation/static/budget flags OFF. Sanity: selector helpers and OSD glyph helpers are in SRAM (`resolution_selector_render_full=0x20000110`, `fast_osd_clear=0x20000298`, `fast_osd_putc_color=0x200002ec`, `fast_osd_puts_color=0x20000410`); `.scratch_x=1688`, `.stack1_dummy=2048`, `.scratch_y=212`, no overlap. Flash confirmed by UF2 progress and reboot.
2026-04-27 12:47:30 -03 - User reported production-like RAM-selector build is not stable. Reflashed previous known-stable isolation firmware for A/B check: `build_reboot_switch_no720p_audio_static_queue_ram_selector_ui/src/neopico_hd.uf2` (selector UI in SRAM, 720p selectable OFF, OSD background disabled, audio static queued silence enabled with work/packet budgets). Flash confirmed by UF2 progress and reboot.
2026-04-27 12:56:47 -03 - User confirmed `build_reboot_switch_no720p_audio_static_queue_ram_selector_ui` is stable. New hypothesis/test idea: pre-seed the selector glyphs into the OSD buffer and allow only OSD visibility toggle, with no interactive menu/update path, to isolate static OSD overlay scanline cost from menu input/render/update work.
2026-04-27 12:59:42 -03 - Added off-by-default `NEOPICO_EXP_STATIC_OSD_TOGGLE_ONLY`: pre-renders reboot selector OSD once in init and makes Core 1 OSD background only debounce MENU and toggle `osd_visible`; BACK/apply/selection/render updates are disabled. Built and flashed `build_reboot_switch_no720p_audio_static_queue_ram_selector_ui_static_osd_toggle/src/neopico_hd.uf2` from previous stable static-queue/RAM-selector settings, with OSD background re-enabled only for static toggle. Flags: static queued silence ON, audio work/packet budgets ON, RAM selector UI ON, 720p selector OFF, OSD background OFF flag disabled. Sanity: `menu_diag_experiment_tick_background=0x60`, render helpers in SRAM, `.scratch_x=1688`, `.stack1_dummy=2048`, `.scratch_y=212`; flash confirmed by UF2 progress and reboot.
2026-04-27 13:06:54 -03 - User confirmed static OSD toggle with static queued audio isolation is still stable. Next A/B: keep `NEOPICO_EXP_STATIC_OSD_TOGGLE_ONLY=ON` and `NEOPICO_EXP_RAM_SELECTOR_UI=ON`, but restore normal dynamic audio by disabling static silence and audio work/packet budget flags.
2026-04-27 13:07:49 -03 - Built and flashed normal-audio static OSD toggle test from `build_reboot_switch_no720p_ram_selector_ui_static_osd_toggle_audio/src/neopico_hd.uf2`. Flags: OSD ON, reboot switch ON, 720p selectable OFF, RAM selector UI ON, static OSD toggle ON, OSD/audio background enabled, normal dynamic audio restored (`AUDIO_PUSH_STATIC_SILENCE=OFF`, audio work/packet budgets OFF, no audio isolation flags). Sanity: `menu_diag_experiment_tick_background=0x60`, `audio_output_callback=0xb8`, `audio_subsystem_background_task=0xbc`, render helpers in SRAM, `.scratch_x=1688`, `.stack1_dummy=2048`, `.scratch_y=212`; flash confirmed by UF2 progress and reboot.
2026-04-27 13:16:17 -03 - User confirmed normal-audio static OSD toggle build is stable. Conclusion: normal dynamic audio plus static OSD visibility is stable; instability is likely in the interactive OSD/menu update path, especially runtime clear/full redraw or selection/apply handling, not in scanline overlay visibility itself.
2026-04-27 13:39:27 -03 - User approved next OSD isolation. Plan: add off-by-default static select-only mode that pre-renders selector once, keeps normal dynamic audio, uses MENU only for OSD show/hide, uses BACK only to move selection with minimal glyph updates, and disables apply/reboot/full redraw at runtime.
2026-04-27 13:41:10 -03 - Added off-by-default `NEOPICO_EXP_STATIC_OSD_SELECT_ONLY`: pre-renders selector once, MENU toggles visibility, BACK moves selection with minimal glyph updates, and apply/reboot/full redraw are disabled at runtime. Built and flashed `build_reboot_switch_no720p_ram_selector_ui_static_osd_select_audio/src/neopico_hd.uf2`: normal dynamic audio ON, OSD/audio background ON, RAM selector UI ON, static select-only ON, static toggle-only OFF, 720p selector OFF. Sanity: `menu_diag_experiment_tick_background=0x1c0`, `audio_output_callback=0xb8`, `audio_subsystem_background_task=0xbc`, OSD glyph helpers in SRAM, `.scratch_x=1688`, `.stack1_dummy=2048`, `.scratch_y=212`; flash confirmed by UF2 progress and reboot.
2026-04-27 14:08:43 -03 - User confirmed static select-only build is stable. Conclusion: normal audio, static OSD visibility, and small BACK-driven glyph updates are stable. The remaining suspect for previous production-like instability is runtime full clear/redraw on open/apply path and/or the reboot apply path, not incremental glyph updates.
2026-04-27 14:16:35 -03 - User approved next split: add a flag-gated pre-rendered selector mode where MENU toggles visibility when hidden, MENU applies/reboots when visible, BACK updates only changed selection glyphs, and runtime full clear/redraw remains disabled.
2026-04-27 14:18:24 -03 - Added off-by-default `NEOPICO_EXP_STATIC_OSD_APPLY`: pre-renders selector once, MENU shows when hidden, MENU applies/reboots when visible, BACK moves selection with minimal glyph updates, and runtime full clear/redraw stays disabled. Built and flashed `build_reboot_switch_no720p_ram_selector_ui_static_osd_apply_audio/src/neopico_hd.uf2`: normal dynamic audio ON, OSD/audio background ON, RAM selector UI ON, static apply ON, static select-only/toggle-only OFF, 720p selector OFF. Sanity: `menu_diag_experiment_tick_background=0x1e0`, `video_pipeline_request_reboot_mode` present, OSD glyph helpers in SRAM, `.scratch_x=1688`, `.stack1_dummy=2048`, `.scratch_y=212`; flash confirmed by UF2 progress and reboot.
2026-04-27 15:20:55 -03 - User reported static-apply build eventually drops sync even without interacting or opening OSD. This points away from the apply/reboot runtime path itself and back toward binary/code-layout sensitivity or the slightly larger always-running OSD background tick (`0x1e0` vs stable select-only `0x1c0`). Next step: compare stable select-only and failing apply binaries before changing/flashing.
2026-04-27 15:24:18 -03 - Added off-by-default `NEOPICO_EXP_RAM_OSD_APPLY_PATH`: forces cold `resolution_selector_apply()` and `video_pipeline_request_reboot_mode()` into SRAM/noinline so the always-running OSD tick does not inline the reboot path. Built and flashed `build_reboot_switch_no720p_ram_selector_ui_static_osd_apply_audio_ram_apply/src/neopico_hd.uf2`: same static-apply behavior as failing build, normal dynamic audio ON, plus RAM apply path ON. Sanity: `resolution_selector_apply=0x20000110`, `video_pipeline_request_reboot_mode=0x20000140`, `menu_diag_experiment_tick_background=0x1cc`; hot flash symbols moved close to stable select-only (`audio_output_callback` +0x0c, `hstx_di_queue_push` +0x10). `.scratch_x=1688`, `.stack1_dummy=2048`, `.scratch_y=212`; flash confirmed after one BOOTSEL retry and reboot.
2026-04-27 15:51:26 -03 - User reported RAM-apply build appears stable so far, but 240p aspect ratio is badly wrong. Investigation found 240p mode renders 320 source pixels to 1280 active pixels as expected, but both rt and non-rt pico_hdmi paths generate AVI InfoFrames with `pixel_repetition=0`. pico_hdmi docs say true 240p needs AVI PR=3 so sinks interpret 1280x240 as 320x240; missing PR likely explains the wide/busted aspect.
2026-04-27 15:53:27 -03 - Fixed 240p AVI pixel repetition in pico_hdmi: rt path now passes `pixel_repetition=3` when `v_active_lines==240 && h_active_pixels==1280`; non-rt `VIDEO_MODE_320x240` path also passes PR=3. Rebuilt and flashed `build_reboot_switch_no720p_ram_selector_ui_static_osd_apply_audio_ram_apply/src/neopico_hd.uf2` with the PR fix. Sanity: disassembly shows 240p branch calls `hstx_packet_set_avi_infoframe(..., pixel_repetition=3)`, scratch unchanged (`.scratch_x=1688`, `.stack1_dummy=2048`, `.scratch_y=212`), hot symbols shifted only ~0x18 from prior RAM-apply build; flash confirmed by UF2 progress and reboot.
2026-04-27 15:58:54 -03 - User reported 240p aspect is still badly busted after PR=3 fix. Read-only follow-up found pico_hdmi docs list AVI InfoFrame completeness as a known compatibility gap: current packet still leaves active-format valid unset and picture aspect unset. Next candidate: set AVI A=1 and R=8, with M=4:3 for 240p/480p and M=16:9 for 720p, while keeping 240p PR=3.
2026-04-27 16:00:47 -03 - Added AVI aspect metadata in pico_hdmi packet builder: A=1, R=8, M=4:3 for non-720p and M=16:9 for VIC 4/720p; 240p still passes PR=3. Rebuilt/flashed `build_reboot_switch_no720p_ram_selector_ui_static_osd_apply_audio_ram_apply/src/neopico_hd.uf2`. Sanity: disassembly shows `hstx_packet_set_avi_infoframe` writes PB1=0x10 and PB2=0x18/0x28, 240p branch still passes PR=3, scratch unchanged. If this remains wrong, likely sink ignores PR/aspect for VIC=0 and next fallback is centered 3x rendering inside 1280-wide 240p timing.
2026-04-27 16:02:59 -03 - User reported 240p still looks wrong after AVI PR/aspect metadata. Requested direct default 240p to rule out reboot switching. Configured `build_240p_osd_direct_boot`: `NEOPICO_VIDEO_240P=ON`, reboot switch OFF, OSD ON, normal dynamic audio, rt HDMI path, no selector/apply experiments.
2026-04-27 16:03:52 -03 - Built and flashed direct-boot 240p A/B from `build_240p_osd_direct_boot/src/neopico_hd.uf2`: `NEOPICO_VIDEO_240P=ON`, reboot switch OFF, OSD ON, normal dynamic audio ON, rt HDMI path. Sanity: flags confirm no reboot selector/switch experiments; `video_pipeline_scanline_callback` direct 240p path in scratch X, `video_pipeline_quadruple_pixels_fast` in scratch Y, `.scratch_x=1672`, `.stack1_dummy=2048`, `.scratch_y=212`; 240p AVI branch still passes PR=3. Flash confirmed by UF2 progress and reboot.
2026-04-27 16:09:01 -03 - User reported direct-boot 240p looks good, confirming 240p timing/InfoFrame are OK and the aspect bug is in reboot-switch rendering. Root cause found: when reboot switch is ON and 720p selector is OFF, `video_pipeline_init()` installed `video_pipeline_scanline_callback_480p` even for active 240p. Patched that callback to be 240p/480p-aware without enabling 720p: it checks active mode, maps 240p lines 1:1, uses `video_pipeline_quadruple_pixels_fast`, and scales OSD spans by 4x. Rebuilt/flashed `build_reboot_switch_no720p_ram_selector_ui_static_osd_apply_audio_ram_apply/src/neopico_hd.uf2`; scratch improved to `.scratch_x=1544`, `.stack1_dummy=2048`, `.scratch_y=212`. Disassembly confirms 240p path uses quad scaler and 2560-byte active span; flash confirmed by UF2 progress and reboot.
2026-04-27 16:18:16 -03 - User requested commit after confirming the fixed reboot-switch 240p build. Commit scope: pico_hdmi 240p AVI/aspect metadata and opt-in layout helpers, plus neopico-hd flag-gated OSD/audio timing experiments, SRAM apply path, and fixed no-720p reboot callback for 240p/480p.
