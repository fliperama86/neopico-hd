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
2026-04-27 16:28:08 -03 - User requested adding 720p support back. Plan: build a 480p/240p/720p reboot-selector image using the latest stable OSD shape (`NEOPICO_EXP_RAM_SELECTOR_UI=ON`, `NEOPICO_EXP_STATIC_OSD_APPLY=ON`, `NEOPICO_EXP_RAM_OSD_APPLY_PATH=ON`) and `NEOPICO_EXP_REBOOT_MODE_SWITCH_720P=ON`, then validate scratch/layout before flashing.
2026-04-27 16:29:08 -03 - Built and flashed `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_audio_ram_apply/src/neopico_hd.uf2`: OSD ON, reboot selector ON, 720p selectable ON, RAM selector UI ON, static apply ON, RAM apply path ON, normal dynamic audio, RT PicoHDMI path. Layout: `.scratch_x=1792` ends exactly at `.stack1_dummy`, `.scratch_y=212`; 480p hot callback in scratch X, selector/apply helpers in SRAM, 720p/240p reboot-mode callback remains in flash. Flash confirmed by UF2 progress and reboot.
2026-04-27 16:31:00 -03 - User reported the 720p-enabled selector build dropped sync after a while. Layout comparison vs stable no-720p build: no scratch overflow, but `.scratch_x` grows 1544 -> 1792; `dma_irq_handler` shifts `0x200802c0 -> 0x200803b8`; flash hot/background code shifts (`audio_output_callback 0x10001674 -> 0x10001988`, `hstx_di_queue_push 0x10006bfc -> 0x10007034`), and `menu_diag_experiment_tick_background` grows `0x1cc -> 0x22c`. Likely same binary-layout/timing sensitivity, not the 240p renderer fix itself.
2026-04-27 16:37:04 -03 - User clarified sync drops at 480p even without opening OSD. Stronger root cause: enabling 720p selector defines `PICO_HDMI_RT_RUNTIME_MODE_ATTRS=1`, which changes the always-running PicoHDMI scratch-X HSTX line builder even in 480p. `build_line_with_di` grows `0x148 -> 0x240` because it contains both DI-in-hsync and DI-in-back-porch layouts plus runtime polarity symbols, shifting `dma_irq_handler` and DI queue helpers. Next fix should keep 480p/240p hot HSTX line builder identical to stable compile-time DI-in-hsync path and dispatch to a separate 720p builder only when booted in 720p.
2026-04-27 16:39:42 -03 - User disagreed that 720p itself is the issue and hypothesized the selector/callback/layout is the perturbation. Built and flashed vanilla 720p RT firmware with no OSD and no reboot selector from `build_720p_vanilla_no_osd/src/neopico_hd.uf2`: `NEOPICO_VIDEO_720P=ON`, `NEOPICO_ENABLE_OSD=OFF`, `NEOPICO_EXP_REBOOT_MODE_SWITCH=OFF`, `NEOPICO_USE_NONRT_HDMI=OFF`, normal audio. Layout: `.scratch_x=1368`, `.stack1_dummy=2048`, `.scratch_y=212`; no selector symbols, `video_pipeline_scanline_callback=0x20080010`, `build_line_with_di=0x200800cc`, `dma_irq_handler=0x2008020c`. Flash confirmed by UF2 progress and reboot.
2026-04-27 16:43:00 -03 - User confirmed vanilla 720p RT no-OSD/no-selector build is stable. Conclusion: 720p output/clock/voltage are not the current instability root. The unstable 720p-selector build is caused by selector/reboot-switch code/layout or by runtime-mode-attribute HSTX hot-path perturbation while in 480p. Next direction: keep 480p idle/hot path layout identical to stable no-720p selector build and isolate 720p-only code behind cold boot-mode-specific paths.
2026-04-27 21:25:59 -03 - Implemented PicoHDMI RT layout-preservation fix for 720p selector builds. Runtime-attribute builds now keep `build_line_with_di()` specialized for stable negative-sync DI-in-HSYNC 480p/240p, while a separate `build_line_with_di_backporch()` in scratch-Y is selected only for 720p/back-porch DI modes. Rebuilt no-720p and 720p selector images. Post-change layout: no-720p selector `.scratch_x=1536`, 720p selector `.scratch_x=1544`, `.scratch_y=520`; both have `video_pipeline_scanline_callback_480p=0x20080020`, `build_line_with_di=0x20080178`, `dma_irq_handler=0x200802b4`. Flashed corrected `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_audio_ram_apply/src/neopico_hd.uf2`; flash confirmed by UF2 progress and reboot.
2026-04-27 21:30:14 -03 - User reported the corrected 720p selector still desyncs. Remaining diff vs stable no-720p is not major scratch-X layout but always-running flash/background placement: `audio_output_callback` and `hstx_di_queue_push` still shifted, and the 720p selector tick is larger. Patched static OSD selector idle path so when OSD is hidden it only polls MENU and returns before timestamp/BACK handling; also placed `menu_diag_experiment_tick_background()` in SRAM under existing `NEOPICO_EXP_RAM_SELECTOR_UI`. Rebuilt 720p selector: `.scratch_x=1544`, `.scratch_y=520`, `menu_diag_experiment_tick_background=0x20000140`, `audio_output_callback=0x1000175c`. Flash attempts failed because no RP-series device was reachable in BOOTSEL or USB serial; image is built at `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_audio_ram_apply/src/neopico_hd.uf2` but not flashed yet.
2026-04-27 21:37:22 -03 - User reported the SRAM-idle-tick 720p selector build also desyncs quickly, then asked for a summary. Before the summary request, a DVI-only/no-audio 720p-selector isolation directory was configured at `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_dvi_only`, but it was not built or flashed.
2026-04-27 21:38:46 -03 - User asked for read-only review of the 480p/240p/720p selector instability and explicitly said not to change code yet. Scope for this pass: inspect current dirty diffs/docs/build metadata only; no code changes, no builds, no flashing.
2026-04-27 21:38:46 -03 - Read-only finding: current 720p-enabled PicoHDMI split keeps 480p/240p `build_line_with_di()` contents/layout close to no-720p, but `BUILD_LINE_WITH_DI` becomes an indirect function-pointer call in the DMA ISR. No-720p disassembly directly `bl build_line_with_di`; 720p-enabled disassembly loads `rt_build_line_with_di` and `blx`es it for audio DI lines. DVI-only next test still useful; if DVI is stable but HDMI/audio-disabled is unstable, suspect DI ISR dispatch/build timing more than dynamic audio background.
2026-04-28 09:57:00 -03 - User approved proceeding with the DVI-only 720p selector isolation. Plan: build configured `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_dvi_only`, inspect layout/symbols, then flash only if a Pico/BOOTSEL target is available. No code changes planned.
2026-04-27 21:46:33 -03 - Built DVI-only 720p selector isolation: `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_dvi_only/src/neopico_hd.uf2` (`NEOPICO_VIDEO_DVI_ONLY=ON`, reboot selector 720p ON, RAM selector UI/static apply/RAM apply path ON). Size: `.text=42984`, `.bss=341932`; `.scratch_x=0x608`, `.scratch_y=0x208`; `menu_diag_experiment_tick_background=0x20000140`. Initial mass-storage `cp` to `/Volumes/RP2350` hung; user requested using `pi flash` next time. Flashed successfully with `gtimeout 60 /Users/dudu/Projects/pico/pi flash ...`; picotool load reached 100% and rebooted into application mode.
2026-04-27 22:04:29 -03 - User reported DVI-only 720p selector isolation has been stable for a while and resolution switching worked. Current sink for this test is a RetroTINK-4K, so note sink changed versus earlier direct display testing. Result points toward HDMI Data Island/audio path or sink-specific HDMI handling rather than selector UI/runtime mode switching alone.
2026-04-27 22:05:25 -03 - User approved next split: build/flash HDMI 720p selector with audio background disabled but Data Islands still active. Goal is to separate baseline HDMI/DI scheduling from dynamic audio capture/SRC/packet generation. Use `pi flash`, not mass-storage copy.
2026-04-27 22:07:11 -03 - Built and flashed HDMI 720p selector with audio background disabled: `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_hdmi_no_audio_bg/src/neopico_hd.uf2`. Flags: DVI-only OFF, `NEOPICO_EXP_DISABLE_AUDIO_BACKGROUND=ON`, reboot selector 720p ON, RAM selector UI/static apply/RAM apply path ON, RT PicoHDMI runtime attrs ON. Layout: `.text=42968`, `.bss=341932`, `.scratch_x=0x608`, `.scratch_y=0x208`, `menu_diag_experiment_tick_background=0x20000140`, `rt_build_line_with_di` present. Flashed via `gtimeout 60 /Users/dudu/Projects/pico/pi flash ...`; load reached 100% and rebooted into application mode.
2026-04-27 22:20:03 -03 - User reported HDMI 720p selector with audio background disabled remains stable in both 480p and 720p on the RetroTINK-4K. Conclusion: selector switching, 720p mode, and baseline HDMI/Data Island scheduling are stable when Core 1 audio background work is removed. Next isolation should re-enable audio background with packet output/capture stages selectively disabled (`NEOPICO_EXP_AUDIO_NO_CAPTURE_START`, `NEOPICO_EXP_AUDIO_CAPTURE_ONLY`, `NEOPICO_EXP_AUDIO_PROCESS_NO_OUTPUT`, `NEOPICO_EXP_AUDIO_ENCODE_NO_QUEUE`, or limiting flags) to locate which audio stage perturbs sync.
2026-04-27 22:35:26 -03 - User noticed current HDMI/no-audio-background 720p selector build does not sync in 240p. 480p and 720p remain stable. Need isolate whether this is 240p HDMI/InfoFrame/Data-Island behavior on RetroTINK-4K, a 240p reboot-mode callback regression in the 720p selector build, or a consequence of suppressing audio background packets.
2026-04-27 22:37:45 -03 - User approved narrow 240p routing fix. Patched `video_pipeline_init()` so 240p reboot mode uses the existing SRAM `video_pipeline_scanline_callback_480p` even when `NEOPICO_EXP_REBOOT_MODE_SWITCH_720P=ON`; only 720p uses flash/shared `video_pipeline_scanline_callback_reboot_modes`. Rebuild/flash same HDMI/no-audio-background selector next.
2026-04-27 22:38:31 -03 - Rebuilt and flashed patched HDMI/no-audio-background 720p selector: `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_hdmi_no_audio_bg/src/neopico_hd.uf2`. Verified layout: `video_pipeline_scanline_callback_480p=0x20080020`, `video_pipeline_scanline_callback_reboot_modes=0x10000e34`, `build_line_with_di=0x20080178`, `dma_irq_handler=0x200802b4`, `.scratch_x=1544`, `.scratch_y=520`. Flash via `gtimeout 60 /Users/dudu/Projects/pico/pi flash ...` reached 100% and rebooted into application mode.
2026-04-27 22:40:07 -03 - User reported patched HDMI/no-audio-background 720p selector still does not sync in 240p. Callback routing was not sufficient. Since direct-boot 240p was good and 480p/720p in current HDMI/no-audio build are stable, strongest current suspect is 240p HDMI Data Island/audio cadence/metadata with RetroTINK-4K: 240p sends HDMI audio packets much more frequently (~3.05 samples/line, one 4-sample packet about every 1.3 lines) than 480p/720p even when audio background is disabled because `hstx_di_queue_get_audio_packet()` falls back to pre-encoded silence.
2026-04-27 22:40:07 -03 - Read-only compare: prior direct-boot 240p build used compile-time 240p (`NEOPICO_VIDEO_240P=ON`, reboot selector OFF, `PICO_HDMI_RT_RUNTIME_MODE_ATTRS` absent) with normal audio background ON and `video_pipeline_scanline_callback=0x20080020`. Current selector uses runtime mode attrs and an indirect DI builder pointer. Next clean A/B: reflash direct-boot 240p on the current RetroTINK-4K to verify sink support; if direct 240p locks, add/test a flag to suppress runtime fallback HDMI audio packets/null-only DI in 240p selector.
2026-04-27 22:43:00 -03 - Flashed existing direct-boot 240p A/B artifact `build_240p_osd_direct_boot/src/neopico_hd.uf2` with `pi flash` (no rebuild). Flags from build: `NEOPICO_VIDEO_240P=ON`, reboot selector OFF, DVI-only OFF, normal audio background ON, RT HDMI without runtime-mode attrs. Layout: `.scratch_x=1672`, `.scratch_y=212`, `video_pipeline_scanline_callback=0x20080020`, `build_line_with_di=0x200801f8`, `dma_irq_handler=0x20080340`. Flash reached 100% and rebooted into application mode. Awaiting whether RetroTINK-4K locks to direct 240p.
2026-04-27 22:43:42 -03 - User reported direct-boot 240p also has no sync on the RetroTINK-4K. Conclusion: current 240p failure is not caused by selector UI, runtime reboot plumbing, or the 720p selector callback. It is sink/timing/HDMI-packet compatibility for 240p on this RetroTINK-4K path. Next A/B should be direct-boot 240p DVI-only/no-audio: if it locks, HDMI Data Islands/audio metadata are the issue; if it still fails, the base 1280x240/15.7 kHz timing is not accepted by this sink.
2026-04-27 22:48:06 -03 - Built and flashed direct-boot 240p DVI-only A/B: `build_240p_dvi_only/src/neopico_hd.uf2`. Flags: `NEOPICO_VIDEO_240P=ON`, `NEOPICO_VIDEO_DVI_ONLY=ON`, reboot selector OFF, OSD ON, RT HDMI path. Layout: `.text=26744`, `.bss=339568`, `.scratch_x=1664`, `.scratch_y=212`, `video_pipeline_scanline_callback=0x20080020`, `build_line_with_di=0x200801f8`, `dma_irq_handler=0x20080334`. Flashed via `gtimeout 60 /Users/dudu/Projects/pico/pi flash ...`; load reached 100% and rebooted into application mode.
2026-04-27 22:49:34 -03 - User reported direct-boot 240p DVI-only looks fine on the RetroTINK-4K. Conclusion: base 1280x240/15.7 kHz timing and renderer are accepted; failure is specifically HDMI Data Islands/audio/InfoFrame behavior in 240p. Practical next fix: make 240p run DVI-style/no Data Islands while keeping normal HDMI for 480p and 720p, ideally behind an off-by-default flag such as `NEOPICO_EXP_DVI_ONLY_240P`.
2026-04-27 22:55:34 -03 - User cautioned that 240p with audio has worked before, so do not settle on permanent 240p DVI-only. Continue investigation by splitting HDMI components: first test 240p HDMI with AVI/ACR/audio InfoFrames still present but continuous audio sample packets suppressed (no real queued packets and no fallback silence packets). If that locks, suspect 240p audio packet cadence; if not, suspect InfoFrame/ACR/AVI metadata or DI placement itself.
2026-04-27 22:57:45 -03 - Added off-by-default diagnostic `NEOPICO_EXP_NO_FALLBACK_AUDIO_PACKET`, which defines `PICO_HDMI_NO_FALLBACK_AUDIO_PACKET=1` for pico_hdmi so an empty DI audio queue returns NULL instead of fallback silent audio packets. Built/flashed direct-boot 240p HDMI isolation `build_240p_hdmi_no_audio_packets/src/neopico_hd.uf2`: `NEOPICO_VIDEO_240P=ON`, DVI-only OFF, reboot selector OFF, OSD ON, `NEOPICO_EXP_AUDIO_PROCESS_NO_OUTPUT=ON`, `NEOPICO_EXP_NO_FALLBACK_AUDIO_PACKET=ON`. Layout: `.text=29776`, `.bss=364900`, `.scratch_x=1656`, `.scratch_y=212`, `video_pipeline_scanline_callback=0x20080020`, `build_line_with_di=0x200801f8`, `dma_irq_handler=0x20080334`, `hstx_di_queue_get_audio_packet=0x2008061c`. Flash via `pi flash` reached 100% and rebooted into application mode.
2026-04-27 22:59:18 -03 - User reported 240p HDMI with audio processing suppressed and fallback audio packets suppressed still has no sync. This rules out continuous audio sample packet cadence. Remaining difference from DVI-only is the command-list path that inserts Data Islands at all: null DI on active/blanking lines plus AVI, ACR, and Audio InfoFrame packets. Next split should suppress Data Island insertion on ordinary active/blanking lines while leaving metadata packets, or conversely suppress metadata and keep null DI, to find which DI class breaks RetroTINK-4K 240p.
2026-04-27 23:01:04 -03 - Added off-by-default diagnostic `NEOPICO_EXP_NO_NULL_DATA_ISLANDS`, defining `PICO_HDMI_NO_NULL_DATA_ISLANDS=1` for pico_hdmi so ordinary active/blanking lines use DVI command lists when no real packet is pending. Built/flashed direct-boot 240p HDMI metadata-only isolation `build_240p_hdmi_metadata_only/src/neopico_hd.uf2`: 240p ON, DVI-only OFF, reboot selector OFF, OSD ON, `NEOPICO_EXP_AUDIO_PROCESS_NO_OUTPUT=ON`, `NEOPICO_EXP_NO_FALLBACK_AUDIO_PACKET=ON`, `NEOPICO_EXP_NO_NULL_DATA_ISLANDS=ON`. Metadata lines (AVI, ACR, Audio InfoFrame) remain. Layout: `.text=29760`, `.bss=364892`, `.scratch_x=1624`, `.scratch_y=212`, `video_pipeline_scanline_callback=0x20080020`, `build_line_with_di=0x200801f8`, `dma_irq_handler=0x20080334`, `hstx_di_queue_get_audio_packet=0x200805f8`. Flash via `pi flash` reached 100% and rebooted.
2026-04-27 23:02:09 -03 - User reported 240p HDMI metadata-only isolation still has no sync. Therefore ordinary null DIs and continuous audio sample packets are not required for the failure. Remaining active difference from DVI-only is metadata packet lines: ACR on vsync/front/back porch cadence, Audio InfoFrame on other vsync lines, and AVI InfoFrame on line 0. Next split: suppress all metadata packet lines too, keeping the firmware in HDMI/non-DVI branch but emitting DVI command lists everywhere; if that locks, reintroduce one metadata class at a time.
2026-04-27 23:03:52 -03 - Added off-by-default diagnostic `NEOPICO_EXP_NO_METADATA_DATA_ISLANDS`, defining `PICO_HDMI_NO_METADATA_DATA_ISLANDS=1` so vsync/blanking metadata packet lines use DVI command lists instead of ACR/Audio InfoFrame/AVI. Built/flashed direct-boot 240p HDMI no-DI isolation `build_240p_hdmi_no_di/src/neopico_hd.uf2`: 240p ON, DVI-only OFF, reboot selector OFF, OSD ON, audio process-no-output ON, no fallback audio packets ON, no null DIs ON, no metadata DIs ON. This keeps the firmware in the HDMI branch but should emit DVI-like command lists everywhere. Layout: `.text=29712`, `.bss=363844`, `.scratch_x=1320`, `.scratch_y=212`, `video_pipeline_scanline_callback=0x20080020`, `build_line_with_di=0x200801f8`, `dma_irq_handler=0x20080334`, `hstx_di_queue_get_audio_packet=0x200804c8`. Flash via `pi flash` reached 100% and rebooted.
2026-04-27 23:04:38 -03 - User reported direct-boot 240p HDMI/no-DI isolation syncs. Hard boundary: 240p base timing, renderer, and HDMI branch are fine; any active metadata Data Island line class (AVI/ACR/Audio InfoFrame as currently emitted) breaks sync on RetroTINK-4K. Next reintroduce metadata one class at a time, with audio packets/null DIs still suppressed: AVI-only first, then ACR-only, then Audio InfoFrame-only.
2026-04-27 23:07:40 -03 - User reported direct-boot 240p HDMI AVI-only isolation syncs on RetroTINK-4K. AVI InfoFrame by itself is accepted. Next isolation: ACR-only, with AVI InfoFrame, Audio InfoFrame, null DIs, and audio sample/fallback packets suppressed.
2026-04-27 23:08:45 -03 - Built and flashed direct-boot 240p HDMI ACR-only isolation `build_240p_hdmi_acr_only/src/neopico_hd.uf2`: 240p ON, DVI-only OFF, reboot selector OFF, OSD ON, audio process-no-output ON, no fallback audio packets ON, no null DIs ON, no AVI InfoFrame ON, no Audio InfoFrame ON, ACR packets enabled. Layout: `.text=29744`, `.bss=364884`, `.scratch_x=1592`, `.scratch_y=212`, `video_pipeline_scanline_callback=0x20080020`, `build_line_with_di=0x200801f8`, `dma_irq_handler=0x20080334`, `hstx_di_queue_get_audio_packet=0x200805dc`. Flash via `pi flash` reached 100% and rebooted.
2026-04-27 23:14:06 -03 - User reported ACR-only isolation did not sync and pointed out v0.1.5 had working 240p with audio. Reset investigation to release-baseline comparison instead of packet-class loop. Release v0.1.5 (`0a02585`) rebuilt standard `neopico_hd_240p.uf2`; tag points PicoHDMI at `2779ce9`, while current checkout is six main commits later with PicoHDMI `1d1247e` plus local DI diagnostic edits. Next control: flash exact v0.1.5 `neopico_hd_240p.uf2` and verify same RetroTINK path.
2026-04-27 23:14:51 -03 - Downloaded official GitHub release asset `neopico_hd_240p.uf2` from v0.1.5 to `/tmp/neopico-hd-v0.1.5/`; SHA-256 verified as `2b54d032e52f6f0c7015f831b1149cf86f1266ec5f2d1b1d8cd41b395f133793` matching release metadata. Flashed exact asset with `pi flash`; load reached 100% and rebooted.
2026-04-27 23:16:18 -03 - User confirmed exact v0.1.5 `neopico_hd_240p.uf2` syncs and has audio on RetroTINK-4K. Strong conclusion: 240p HDMI/audio/ACR are not inherently incompatible; current no-sync is a post-v0.1.5 regression. Highest-probability delta is the post-release 240p AVI metadata change (`PB1=0x10`, `PB2=0x18`, PR=3) versus release AVI (`PB1=0x00`, `PB2=0x08`, PR=0). Next test should restore legacy AVI metadata with normal audio, not suppress ACR/audio.
2026-04-27 23:18:16 -03 - Added flag-gated legacy AVI experiment: `NEOPICO_EXP_LEGACY_240P_AVI_INFOFRAME` -> `PICO_HDMI_LEGACY_240P_AVI_INFOFRAME=1`, restoring v0.1.5-style AVI bytes for VIC 0 (`PB1=0x00`, `PB2=0x08`) and disabling 240p PR=3 for this test. Built/flashed `build_240p_hdmi_legacy_avi_normal_audio/src/neopico_hd.uf2`: direct 240p HDMI, OSD ON, reboot selector OFF, normal audio/ACR/audio InfoFrame/null DI/fallback packets all enabled. Layout: `.text=30280`, `.bss=365420`, `.scratch_x=1664`, `.scratch_y=212`, `audio_output_callback=0x100012c4`, `build_line_with_di=0x200801f8`, `dma_irq_handler=0x20080334`. Flash via `pi flash` reached 100% and rebooted.
2026-04-27 23:19:35 -03 - User confirmed `build_240p_hdmi_legacy_avi_normal_audio` works fine on RetroTINK-4K, including sync/audio. Root cause is the post-v0.1.5 240p AVI metadata change, not HDMI audio/ACR: legacy VIC-0 AVI (`PB1=0x00`, `PB2=0x08`, PR=0) works; newer active-format/aspect/PR=3 240p metadata breaks sync on this sink. Next fix should keep 240p on legacy AVI metadata by default or behind a compatibility option, while preserving newer metadata for 480p/720p.
2026-04-27 23:21:51 -03 - Correction/narrowing: do not overstate the culprit as "AVI alone breaks sync." Earlier AVI-only with the newer metadata synced, while ACR-only without AVI did not. The proven A/B is full normal 240p HDMI/audio stream: current newer 240p AVI metadata no-syncs; v0.1.5-style VIC-0 AVI metadata (`PB1=0x00`, `PB2=0x08`, PR=0) syncs with audio. Exact sub-field split between PB1/PB2 and PR still untested.
2026-04-27 23:26:55 -03 - User asked to restore the resolution selector with the 240p metadata lesson applied. Plan: start from the last selector image stable in 480p/720p on RetroTINK-4K (`NEOPICO_EXP_DISABLE_AUDIO_BACKGROUND=ON`, RAM selector UI/static apply/RAM apply path, 720p selector ON) and add `NEOPICO_EXP_LEGACY_240P_AVI_INFOFRAME=ON` only. This tests selector + 240p sync without reintroducing audio-background instability yet.
2026-04-27 23:28:15 -03 - Built/flashed selector restore artifact `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_hdmi_no_audio_bg_legacy_240p_avi/src/neopico_hd.uf2`. Flags: OSD ON, DVI-only OFF, reboot selector ON, 720p selectable ON, RAM selector UI/static apply/RAM apply path ON, audio background disabled ON, legacy 240p AVI ON, no DI/metadata/audio-packet suppressors. PicoHDMI defs: `PICO_HDMI_RT_RUNTIME_MODE_ATTRS=1`, `PICO_HDMI_LEGACY_240P_AVI_INFOFRAME=1`. Layout: `.text=28312`, `.bss=339612`, `.scratch_x=1544`, `.scratch_y=520`, `video_pipeline_scanline_callback_480p=0x20080020`, `build_line_with_di=0x20080178`, `dma_irq_handler=0x200802b4`, `build_line_with_di_backporch=0x200810d4`. Flash via `pi flash` reached 100% and rebooted. Ask user to test 480p/240p/720p switching and 240p sync.
2026-04-27 23:29:34 -03 - User reported selector with legacy 240p AVI looks good across modes but has no audio. This is expected for that artifact because `NEOPICO_EXP_DISABLE_AUDIO_BACKGROUND=ON` was intentionally kept from the prior stable 480p/720p selector baseline. Next A/B: same selector/legacy-AVI flags with audio background enabled.
2026-04-27 23:31:00 -03 - Built/flashed audio-enabled selector with legacy 240p AVI: `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_hdmi_audio_bg_legacy_240p_avi/src/neopico_hd.uf2`. Flags: reboot selector ON, 720p selectable ON, RAM selector UI/static apply/RAM apply path ON, DVI-only OFF, audio background enabled (`NEOPICO_EXP_DISABLE_AUDIO_BACKGROUND=OFF`), legacy 240p AVI ON, no DI/metadata/audio suppressors. PicoHDMI defs: `PICO_HDMI_RT_RUNTIME_MODE_ATTRS=1`, `PICO_HDMI_LEGACY_240P_AVI_INFOFRAME=1`. Layout: `.text=31880`, `.bss=365464`, `.scratch_x=1544`, `.scratch_y=520`, `audio_output_callback=0x10001748`, `video_pipeline_scanline_callback_480p=0x20080020`, `build_line_with_di=0x20080178`, `dma_irq_handler=0x200802b4`, `hstx_di_queue_get_audio_packet=0x200805a4`. Flash via `pi flash` reached 100% and rebooted.
2026-04-27 23:34:14 -03 - User reported audio-enabled selector with legacy 240p AVI "kinda works" but drops sync after a few minutes. This reintroduces the earlier dynamic-audio-background instability, separate from 240p AVI metadata. Next A/B: keep selector and audio enabled, but bound Core 1 audio background/output work per pass (`NEOPICO_EXP_AUDIO_LIMIT_BACKGROUND_WORK=ON`, `NEOPICO_EXP_AUDIO_LIMIT_OUTPUT_PACKETS=ON`) to test whether unbounded audio processing bursts perturb HSTX timing.
2026-04-27 23:35:41 -03 - Built/flashed bounded-audio selector `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_hdmi_audio_limited_legacy_240p_avi/src/neopico_hd.uf2`. Flags: selector 480p/240p/720p ON, RAM selector UI/static apply/RAM apply path ON, legacy 240p AVI ON, audio background enabled, `NEOPICO_EXP_AUDIO_LIMIT_BACKGROUND_WORK=ON`, `NEOPICO_EXP_AUDIO_LIMIT_OUTPUT_PACKETS=ON`, no DI/metadata/audio suppressors. PicoHDMI defs unchanged: `PICO_HDMI_RT_RUNTIME_MODE_ATTRS=1`, `PICO_HDMI_LEGACY_240P_AVI_INFOFRAME=1`. Layout: `.text=31952`, `.bss=365468`, `.scratch_x=1544`, `.scratch_y=520`, `audio_output_callback=0x10001748`, `build_line_with_di=0x20080178`, `dma_irq_handler=0x200802b4`. Flash via `pi flash` reached 100% and rebooted.
2026-04-27 23:36:17 -03 - User reported bounded-audio selector has no video, only audio. Treat stacked audio limiting flags as unsafe for now. Restore last known-good video selector (`build_reboot_switch_720p_ram_selector_ui_static_osd_apply_hdmi_no_audio_bg_legacy_240p_avi`) before further audio A/B; next splits should test one audio mitigation flag at a time.
2026-04-27 23:37:02 -03 - Attempted restore flash of known-good video/no-audio selector `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_hdmi_no_audio_bg_legacy_240p_avi/src/neopico_hd.uf2`. `pi flash` load reached 100% but exited 249 without final reboot confirmation; follow-up `picotool info` reports RP2350 in application/USB-serial mode (`/dev/cu.usbmodem2101`), so artifact likely loaded and booted. Ask user to verify video is restored.
2026-04-27 23:42:26 -03 - Catch-up after late-session scratchbook gap. Key confirmed results: exact GitHub v0.1.5 `neopico_hd_240p.uf2` syncs with audio on RetroTINK-4K; direct current 240p HDMI with legacy v0.1.5-style VIC-0 AVI metadata (`PB1=0x00`, `PB2=0x08`, PR=0) also syncs with audio. Newer 240p AVI metadata (`PB1=0x10`, `PB2=0x18`, PR=3) causes no-sync in the full normal 240p HDMI/audio stream, but earlier AVI-only/ACR-only splits mean do not overstate AVI alone as the sole cause. User agreed 240p is out-of-spec, so conservative VIC-0 metadata is the sound default for 240p.
2026-04-27 23:42:26 -03 - Selector restoration state: `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_hdmi_no_audio_bg_legacy_240p_avi` looked good across 480p/240p/720p but intentionally had no real audio because `NEOPICO_EXP_DISABLE_AUDIO_BACKGROUND=ON`. Audio-enabled selector `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_hdmi_audio_bg_legacy_240p_avi` had audio but user reported it dropped sync after a few minutes. Bounded-audio selector `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_hdmi_audio_limited_legacy_240p_avi` was flashed, then user reported no video/just audio, but now suspects that result was a RetroTINK-4K issue; treat that single no-video observation as inconclusive unless reproduced.
2026-04-27 23:49:40 -03 - User switched debugging sink to Morph4K because it recovers reliably from de-syncs; RetroTINK-4K remains high-quality/final compatibility target but has shown recovery issues during debugging. Interpret near-term sync/drop tests against Morph4K as more reliable for iteration, then recheck final candidate on RT4K.
2026-04-27 23:56:06 -03 - Status checkpoint: `SCRATCHBOOK.md` conflict is resolved with no remaining conflict markers/unmerged files. Parent repo staged files are `SCRATCHBOOK.md`, `src/CMakeLists.txt`, `src/experiments/menu_diag_experiment.c`, and `src/video/video_pipeline.c`; `lib/pico_hdmi` submodule remains dirty (`hstx_data_island_queue.c`, `hstx_packet.c`, `video_output.c`, `video_output_rt.c`). Best next investigation step on Morph4K is to retest/split the audio-limiting selector path with legacy 240p AVI, using `pi flash`.
2026-04-27 23:57:27 -03 - Flashed bounded-audio selector on Morph4K test path: `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_hdmi_audio_limited_legacy_240p_avi/src/neopico_hd.uf2` using `pi flash`. Verified cache flags before flash: selector 720p ON, RAM selector UI/static apply/RAM apply path ON, audio background enabled, `NEOPICO_EXP_AUDIO_LIMIT_BACKGROUND_WORK=ON`, `NEOPICO_EXP_AUDIO_LIMIT_OUTPUT_PACKETS=ON`, legacy 240p AVI ON. Flash reached 100% and rebooted into application mode.
2026-04-27 23:57:59 -03 - User reported bounded-audio selector drops sync after a few seconds on Morph4K. This confirms prior RT4K no-video/just-audio observation was not only sink recovery behavior. Stacking `NEOPICO_EXP_AUDIO_LIMIT_BACKGROUND_WORK=ON` with `NEOPICO_EXP_AUDIO_LIMIT_OUTPUT_PACKETS=ON` is unsafe/worse than the unbounded audio-enabled selector, which dropped after minutes. Next split: test background-work limiting only, leaving packet output cadence unrestricted.
2026-04-28 00:00:19 -03 - User suspects the investigation may be back to binary layout sensitivity. Layout comparison: stable no-audio selector `.text=28312`, `.bss=339612`; audio-enabled selector `.text=31880`, `.bss=365464`; stacked limiter `.text=31952`, `.bss=365468`; background-work-only split built but not flashed `.text=31904`, `.bss=365464`. Hot SRAM addresses (`video_pipeline_scanline_callback_480p`, `build_line_with_di`, `dma_irq_handler`, `hstx_di_queue_get_audio_packet`, 720p backporch builder) remained identical across these builds, so next cleaner A/B is audio code linked/state machine active but capture not started: `NEOPICO_EXP_AUDIO_NO_CAPTURE_START=ON`.
2026-04-28 00:01:29 -03 - Built/flashed layout probe `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_hdmi_audio_no_capture_start_legacy_240p_avi/src/neopico_hd.uf2` with audio background enabled but `NEOPICO_EXP_AUDIO_NO_CAPTURE_START=ON` and all limiter/output-isolation flags OFF. This keeps the audio subsystem/state machine linked and `.bss` in the audio-enabled range (`.text=29472`, `.bss=364424`, `.scratch_x=1544`, `.scratch_y=520`) while not starting I2S capture or dynamic audio packet generation. Flash via `pi flash` reached 100% and rebooted.
2026-04-28 00:14:41 -03 - User reported the `NEOPICO_EXP_AUDIO_NO_CAPTURE_START=ON` layout probe is stable on Morph4K and has no audio, as expected. This argues against pure binary layout or the idle audio state machine as the cause. Next split should start capture/SRC but disable HDMI audio output callback work (`NEOPICO_EXP_AUDIO_PROCESS_NO_OUTPUT=ON`) to determine whether active capture/SRC background work alone can perturb sync.
2026-04-28 00:16:12 -03 - Built/flashed capture/SRC-no-output split `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_hdmi_audio_process_no_output_legacy_240p_avi/src/neopico_hd.uf2`. Flags: selector 720p ON, RAM selector UI/static apply/RAM apply path ON, audio background enabled, `NEOPICO_EXP_AUDIO_NO_CAPTURE_START=OFF`, `NEOPICO_EXP_AUDIO_PROCESS_NO_OUTPUT=ON`, limiter flags OFF, legacy 240p AVI ON. Layout: `.text=31368`, `.bss=364944`, `.scratch_x=1544`, `.scratch_y=520`; hot HSTX/ISR SRAM addresses unchanged. Flash via `pi flash` reached 100% and rebooted. Expected: stable means capture/SRC alone is not enough and failure is in audio encode/queue/output; drop means active audio processing load perturbs video.
2026-04-28 00:20:00 -03 - User asked whether layout sensitivity is technically understood/documented and whether ASM could work around it. Local Pico 2/RP2350 datasheet supports the general mechanism: XIP is external flash behind a 16 kB 2-way cache; cache misses require QMI serial accesses; HSTX FIFO is only 8x32-bit and DMA is capped at one HSTX FIFO write per system clock; BUSCTRL exposes contested/upstream/downstream stall counters, including XIP cache-miss stalls. Current stable `AUDIO_NO_CAPTURE_START=ON` result argues against pure binary layout alone, but layout can still affect XIP misses, SRAM bank/data placement, veneers/literals, and real-time slack. ASM only helps if used with SRAM placement and fixed memory access patterns; ASM in flash still suffers XIP/cache/bus stalls.
2026-04-28 00:24:41 -03 - User reported current `NEOPICO_EXP_AUDIO_PROCESS_NO_OUTPUT=ON` firmware desynced on Morph4K. Interpretation: active audio capture/SRC/background processing is enough to perturb video even when HDMI audio packet encode/queue/output is suppressed. Next split: `NEOPICO_EXP_AUDIO_CAPTURE_ONLY=ON` with `NEOPICO_EXP_AUDIO_NO_CAPTURE_START=OFF` and output/processing flags OFF, to test PIO2/I2S capture + DMA running without draining/SRC/background work.
2026-04-28 00:25:52 -03 - Built/flashed capture-only split `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_hdmi_audio_capture_only_legacy_240p_avi/src/neopico_hd.uf2`. Flags: selector 720p ON, audio background enabled, `NEOPICO_EXP_AUDIO_CAPTURE_ONLY=ON`, `NEOPICO_EXP_AUDIO_NO_CAPTURE_START=OFF`, `NEOPICO_EXP_AUDIO_PROCESS_NO_OUTPUT=OFF`, limiter flags OFF, legacy 240p AVI ON. Layout: `.text=29672`, `.bss=364428`, `.scratch_x=1544`, `.scratch_y=520`; hot HSTX/ISR SRAM addresses unchanged. Flash via `pi flash` reached 100% and rebooted. Expected: stable means PIO2/I2S DMA capture alone is not enough and the SRC/drain loop is the culprit; drop means capture DMA/PIO bus activity itself is destabilizing.
2026-04-28 00:42:01 -03 - Added static binary audit helper `scripts/audit_firmware.py` (read-only runtime). It accepts an ELF or build directory plus optional `--baseline`, reports CMake flags, key section sizes, critical SRAM symbols, Core 1/background flash residency, scratch overflow, SRAM critical functions that branch/call into flash, and section/symbol drift. Initial audits: process-no-output unstable build vs no-capture-start stable baseline has no hard failures but warns that `audio_subsystem_background_task`, `audio_background_task`, `audio_output_callback`, and `audio_pipeline_process` are flash-resident; capture-only build vs no-capture-start baseline has no hard failures and only warns `audio_subsystem_background_task` is flash-resident. Full audio selector vs no-audio selector also has no hard HSTX path failures; enabling audio adds `.text=+3568`, `.bss=+25852` and flash-resident Core 1 audio background/SRC/output symbols while critical HSTX SRAM addresses stay fixed. Syntax checked with `python3 -m py_compile`.
2026-04-28 00:52:48 -03 - User clarified priority: audio is fundamental, resolution selection is optional. Reflashed intentional failing baseline `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_hdmi_audio_bg_legacy_240p_avi/src/neopico_hd.uf2` via `pi flash`; flags include full audio background enabled, selector 480p/240p/720p enabled, RAM selector UI/static apply/RAM apply path enabled, and legacy 240p AVI only for 240p. This baseline is expected to desync, but is the right failure target for narrowing where audio + selector breaks.
2026-04-28 00:55:51 -03 - Added `NEOPICO_EXP_SELECTOR_NAV_ONLY` flag. When enabled, OSD selector rendering/BACK navigation remains active but `resolution_selector_apply()` hides the menu without calling `video_pipeline_request_reboot_mode()`; direct non-selector reboot requests from the menu path are also suppressed. Built/flashed full-audio nav-only selector `build_reboot_switch_720p_ram_selector_ui_static_osd_apply_hdmi_audio_nav_only_legacy_240p_avi/src/neopico_hd.uf2` with audio enabled, selector UI enabled, legacy 240p AVI ON, and nav-only ON. Static audit vs full-audio failing baseline showed no hard HSTX placement failures; `.text`/`.bss` unchanged, `.data=-96`, `menu_diag_experiment_tick_background` shifted slightly in SRAM. Flash via `pi flash` reached 100% and rebooted.
2026-04-28 00:58:46 -03 - User reported nav-only selector still breaks and asked for static OSD with no callbacks. Added `NEOPICO_EXP_STATIC_OSD_VISIBLE` flag to show the pre-rendered selector at boot. Built/flashed `build_reboot_switch_720p_ram_selector_ui_static_osd_visible_no_osd_bg_audio_legacy_240p_avi/src/neopico_hd.uf2` with full audio ON, selector/static OSD ON, nav-only ON, static-visible ON, and `NEOPICO_EXP_DISABLE_OSD_BACKGROUND=ON`; this means the OSD buffer is rendered/shown once at init, but `menu_diag_experiment_tick_background()` is not called from the Core 1 background task. Static audit vs nav-only failing build showed no hard HSTX placement failures; `.text=-16`, `.data=-640`, `.bss` unchanged, critical scratch addresses unchanged. Flash via `pi flash` reached 100% and rebooted.
2026-04-28 01:01:46 -03 - Static OSD/no-callback build was better but still unstable. Built/flashed no-OSD sanity artifact `build_480p_hdmi_audio_no_osd_sanity/src/neopico_hd.uf2`: direct 480p HDMI, audio fully enabled, `NEOPICO_ENABLE_OSD=OFF`, reboot selector OFF, no audio isolation flags. Static audit vs static-OSD build: `.text=-1880`, `.rodata=-1168`, `.data=-428`, `.bss=-58752`, `.scratch_x=-200`, `.scratch_y=-308`; only direct 480p HSTX path remains, no 720p backporch builder or 480p reboot callback symbol. Flash via `pi flash` reached 100% and rebooted. Await user stability/audio result.
2026-04-28 01:12:48 -03 - User noted an older BACK-button resolution cycler had worked and suspects memory placement could be the last straw. Built/flashed old-style 480p/240p BACK cycler without selector UI: `build_reboot_switch_no720p_audio_back_cycler_no_selector_ui_legacy_240p_avi/src/neopico_hd.uf2`. Flags: audio ON, OSD ON only for button polling, reboot switch ON, 720p selector OFF, `NEOPICO_EXP_DISABLE_REBOOT_SELECTOR_UI=ON`, static/selector UI flags OFF, legacy 240p AVI ON. Audit vs no-OSD sanity: `.text=+632`, `.bss=+58716`, `.scratch_x=+192`; critical HSTX symbols still in scratch, but `menu_diag_experiment_tick_background=0x1000036c` is flash-resident. Flash via `pi flash` reached 100% and rebooted. If this fails, next ladder step is same build with only the tiny OSD/background poller moved to SRAM via `NEOPICO_EXP_RAM_SELECTOR_UI=ON`.
2026-04-28 01:21:50 -03 - User reported old-style 480p/240p BACK cycler without selector UI seems stable even after cycling resolution. Built/flashed 720p reintroduction artifact `build_reboot_switch_720p_audio_back_cycler_no_selector_ui_legacy_240p_avi/src/neopico_hd.uf2`: audio ON, OSD button polling ON, reboot switch ON, 720p cycling ON, selector UI disabled, legacy 240p AVI ON. Audit vs stable no-720p cycler: `.text=+944`, `.rodata=+24`, `.data=+4`, `.bss=+32`, `.scratch_x=+8`, `.scratch_y=+308`; adds `build_line_with_di_backporch` in scratch-Y and shifts flash audio/background symbols by about `+0x210`, no hard HSTX placement failures. Flash via `pi flash` reached 100% and rebooted.
2026-04-28 01:29:22 -03 - User reported 720p reintroduction through the old-style cycler is not stable and asked why OSD was brought back. Root issue: the previous cycler still required `NEOPICO_ENABLE_OSD=ON`, so even with selector UI disabled it kept the OSD/menu module and large OSD BSS in the image. Added off-by-default `NEOPICO_EXP_REBOOT_BUTTON_CYCLER` to permit reboot-mode BACK cycling with `NEOPICO_ENABLE_OSD=OFF`; BACK polling now lives in `main.c` and calls `video_pipeline_request_reboot_mode()` directly. Built `build_reboot_switch_720p_audio_bare_back_cycler_no_osd_legacy_240p_avi`: audio ON, OSD OFF, reboot switch ON, 720p cycling ON, bare cycler ON, legacy 240p AVI ON. Audit vs unstable OSD-enabled 720p cycler: `.text=-432`, `.bss=-58708`, `.scratch_x=-184`; no `menu_diag`/`fast_osd` symbols remain in the linked image, while 720p backporch DI builder remains in scratch-Y.
2026-04-28 01:30:38 -03 - Flashed `build_reboot_switch_720p_audio_bare_back_cycler_no_osd_legacy_240p_avi/src/neopico_hd.uf2` with `pi flash`; load reached 100% and rebooted into application mode. Expected behavior: no OSD appears, full audio is enabled, BACK cycles 480p -> 240p -> 720p -> 480p through watchdog reboot requests. This isolates 720p mode inclusion plus audio from OSD/menu code and OSD BSS.
2026-04-28 01:43:08 -03 - User reported bare BACK cycler looks stable. Cleanup/documentation pass: added `docs/REBOOT_RESOLUTION_SWITCHING.md` with stable flags, BACK-cycle behavior, OSD-free rationale, legacy 240p AVI values, 720p DI builder split, and audit helper usage; linked it from README. Removed failed nav-only/static-visible OSD probes and temporary HDMI packet-suppression diagnostics from final source while keeping the stable path (`NEOPICO_EXP_REBOOT_BUTTON_CYCLER`, `NEOPICO_EXP_LEGACY_240P_AVI_INFOFRAME`, 720p backporch DI split). Rebuilt documented stable config and reran audit vs unstable OSD-enabled 720p cycler: `.bss=-58708`, no OSD symbols linked, expected audio background flash-residency warnings only.
2026-04-28 20:26:32 -03 - User reported the OSD/late-BSS sanity branch still did not work, then stashed local changes and verified `./flash` default firmware works. With the tree clean at `d1faed3`, flashed OSD-free cycler artifact `build_reboot_switch_720p_audio_bare_back_cycler_no_osd_legacy_240p_avi/src/neopico_hd.uf2` via `pi flash`; flags audited as OSD OFF, audio ON, reboot mode switch ON, 720p cycling ON, bare BACK cycler ON, legacy 240p AVI ON. Flash reached 100% and rebooted into application mode.

## 2026-06-11 — pico_hdmi 2.0-beta upgrade (in progress)
- Upstream `2.0-beta` branch now contains: opt-in precomposed scanout + native pixel mode, ISR-side audio island pacing, audio spread across all scanlines, silence-packet IEC B-flag fix + underrun counter (`hstx_di_queue_silence_count`), `hstx_packet_set_audio_samples_cs` (proper channel status, V=0), `video_output_force_resync` + resync counter.
- The two previously submodule-only commits (240p AVI metadata `1d1247e`, reboot-mode switching `fc4713c`) were cherry-picked onto 2.0-beta upstream (small conflicts resolved additively) — 2.0-beta is now a strict superset; the dangling-submodule-pointer problem is also fixed.
- Submodule updated to `7fa6dd5` (2.0-beta head). **Both builds compile clean with ZERO app changes**: `build/` (480p, RUNTIME_MODES=ON) and `build-720p-nonrt/` (720p, RUNTIME_MODES=OFF).
- NOT yet committed (house rule: no commits without explicit OK). Not yet flashed/tested on hardware.
- Adoption ladder (all opt-in, flag-gated per house policy):
  1. Free with the bump: silence-packet B-flag fix (audio underruns no longer reset sink IEC block sync).
  2. Small: switch audio encode to `hstx_packet_set_audio_samples_cs` (proper 48 kHz consumer channel status, V=0 as required); surface `hstx_di_queue_silence_count` on the OSD (SRC underrun observability).
  3. Medium: precomposed scanout + native pixel mode on the non-RT path (`PICO_HDMI_PRECOMPOSED_ACTIVE_LINES`) — ISR shrinks to pointer swap + island patch; NOTE: OSD compositing currently lives in the doubling phase of the ISR and must move (canvas-row pattern from rp2350-doom).
  4. Large: port precomposed mode into `video_output_rt.c` → candidate for stable RT 720p.
- Caution from rp2350-doom: never printf on the HDMI core's background task; validate audio changes with a pure sine + spectrogram (square waves mask dropped-sample artifacts).

## 2026-06-11 — Phase 2: precomposed/native integration (built, NOT committed, NOT hardware-tested)
- New flag `NEOPICO_EXP_PRECOMPOSED_HDMI` (OFF default; requires `NEOPICO_USE_NONRT_HDMI=ON`, 480p-only — native pixel mode is a 2x doubler, so 720p/240p excluded for now).
- When ON: pointer-model scanline callback in `video_pipeline.c` — video rows return the captured `line_ring` line DIRECTLY (zero copy; hardware does the 2x), letterbox/no-signal rows return static native color lines, OSD rows return a ping/pong native scratch composed once per source line (explicit word loops, no memcpy in ISR context; cache invalidated per frame in vsync cb). `video_output_set_native_pixel_mode(true)` + 8-entry compose ring; `video_output_compose_service()` added at top of `combined_background_task` (one-time header build). Audio pump unchanged — the library ISR now pops/patches islands itself (starvation-proof).
- RAM cost ~5 KB (ring 2.5K + scratches). Builds: `build/` and `build-720p-nonrt/` regression-clean (flag off, zero diffs in behavior); new `build-precomp/` (480p non-RT + flag ON) compiles clean, no warnings.
- NEXT: flash `build-precomp/src/neopico_hd.uf2` on MVS hardware. Expect: identical picture (incl. OSD toggle), identical audio; wins are ISR headroom + audio starvation immunity. Validate audio with a sustained tone source if possible (square-ish content masks dropped-sample artifacts — rp2350-doom lesson). If good → consider making it the default 480p path, then tackle 720p (precomposed islands apply as-is; pixel path needs the 3x pre-expansion strategy or stays copy-model).

## 2026-06-11 — Phase 2 hardware verdict
- `build-precomp` flashed on MVS hardware: picture/OSD/audio stable ("stable enough" — user). Committing the flag-gated integration; flag remains OFF by default. Longer soak + sustained-tone audio check still recommended before making it the default 480p path.

## 2026-06-11 — Root OSD menu (built, NOT committed, NOT hardware-tested)
- New flag `NEOPICO_OSD_ROOT_MENU` (OFF default; requires OSD + at least one of SELFTEST / REBOOT_MODE_SWITCH). Root menu: "Resolution" (when reboot-switch compiled) + "Self Test" (when selftest compiled). BACK cycles, MENU enters; leafs return to root on MENU (Resolution applies+reboots on a changed selection, returns to root when selection==current); root auto-hides after 8 s idle.
- Lifts the old selector⊕selftest exclusivity under the flag (NEOPICO_REBOOT_SELECTOR_UI loses the !SELFTEST clause when ROOT_MENU on); legacy single-screen behaviors fully preserved when flag off.
- Layout-safety verified per house history: ALL new code in menu_diag_experiment.c (Core 1 background, flash); flag-off UF2 byte-identical (sha 95d15e6e); zero menu symbols in scratch sections; scratch_x 0x6c0 (precomp+selftest+menu) / 0x610 (rt+selector+selftest+menu), both < 0x800 boundary; capture path and scanline callbacks untouched.
- Builds verified: build-precomp (precomposed + selftest + root menu) and rt-fullmenu (RT + 240p/480p/720p selector + selftest + root menu). NEXT: flash build-precomp first (menu w/ Self Test only), then the rt-fullmenu for the full two-entry experience; soak per usual.

## 2026-06-11 — Precomposed soak FAILED: sync drop after ~20-30 min
- `build-precomp` (precomposed + native pixel + selftest OSD) dropped sync after ~20-30 min. No auto-recovery (NeoPico does not wire `video_output_force_resync`; the lib has it).
- CROSS-PROJECT SIGNAL: rp2350-doom (same lib mode) logs rare HSTX desyncs too — masked there by a frame-pacing watchdog ("brownouts", rp2350-doom issue #1). Two consumers, same rare failure → suspect the new mode itself. Prime candidate: the per-post 16/32-bit `al1_ctrl` swap in the precomposed/native ISR racing a late or coalesced DMA IRQ (one mis-sized post permanently desyncs the HSTX expander command stream).
- Hardware caveat: this bench uses an OLDER PCB revision (user notes it could interfere, though unlikely).
- BISECTION LADDER (user running step 1):
  1. Known-good firmware, ≥1 h soak → establishes PCB/baseline health.
  2. If clean: lib-2.0-bump-but-flag-OFF build (`build/` after 981efab), ≥1 h → isolates the lib upgrade itself (expected clean; flag-off paths unchanged).
  3. If clean: precomposed again → failure confirms the new mode; then (a) port doom's frame-pacing watchdog + force_resync as mitigation, (b) fix the ctrl-swap race properly in pico_hdmi (e.g. eliminate per-IRQ transfer-size switching: dedicated channel config per post type, or derive size from DMA state not software flags).
- OSD root-menu work is built+verified (layout-neutral) but parked until the sync question settles.

## 2026-06-11 — Rung-3 instrumented firmware ready (not flashed)
- `build-precomp` reconfigured: root menu OFF (match the failing config), precomposed + selftest + NEW frame-pacing watchdog (>12 vsyncs/100 ms -> video_output_force_resync, counter g_neopico_resync_count) + "RS n" readout on selftest row 15 (green 0 / yellow nonzero). Scratch_x unchanged 0x6c0.
- New diag knob NEOPICO_EXP_STRESS_CORE1_US (CMake, default 0): busy-wait per Core 1 background tick to accelerate IRQ-timing repro; off in this build.
- Plan: after rung 2 (lib-bump flag-off, soaking now) reports clean, flash this; RS counter turns hour-soaks into glance-readable event tallies. If RS ticks: stress build next to confirm rate scales with Core 1 load, then fix the 16/32-bit ctrl swap race in pico_hdmi properly.

## 2026-06-11 22:42 — Bisection rungs 1+2 PASSED
- Rung 1: v0.6.0 release fw, >1 h clean (PCB + bench exonerated).
- Rung 2: lib-2.0-beta bump, all flags off, ~1 h clean (library upgrade exonerated).
- Remaining suspect: precomposed/native mode. Flashing rung-3 instrumented build (watchdog + RS counter on selftest row 15).

## 2026-06-11 — LAYOUT SENSITIVITY REPRODUCED with a controlled diff
- Rung-3 instrumented build (watchdog + RS renderer + root-menu code, ~394 lines, ALL in flash/background path, ZERO scratch-section or capture-path changes) was "super glitchy" immediately; persisted after fixing a real watchdog rate bug. Reverting ONLY those 394 lines (back to commit 3d1cb91 state) restored calm on the same bench, same session.
- Implication: the perturbation mechanism is NOT scratch occupancy and NOT the code's execution (watchdog fix changed behavior, glitch unchanged). Prime suspect: flash/XIP placement+alignment shifts of hot non-scratch code moved by the added 394 lines. Future experiments: re-add the code in halves; try forced alignment / pinning hot functions to RAM; diff .map hot-symbol addresses between calm and glitchy builds.
- Patch preserved at /tmp/neopico-rootmenu-watchdog.patch (copy into repo notes if needed).
- Current firmware on bench: commit 3d1cb91 state, flags NONRT+PRECOMPOSED+OSD+SELFTEST (the same source as the earlier "looked good" soak that dropped at ~20-30 min). Soak continues to characterize the original drop.

## 2026-06-12 morning — Run #6 (pure phase 2 baseline) PASSED ~8 h overnight
- Precomposed render path exonerated; canonical baseline established (sha 76c1233d). OSD/selftest-compiled implicated (3/3 drops 20-60 min vs 0 drops in 8 h without). Next: experiment #7 (OSD compiled, never opened) to split presence-vs-opening. Details in STATE.md.

## 2026-06-12 — XIP CONVICTED; copy-to-RAM is the structural fix
- Oracle: instant-glitch code (394-line patch) + NEOPICO_COPY_TO_RAM=ON -> CALM. Only variable: flash vs RAM execution. XIP fetch-stall timing was the mechanism behind the layout sensitivity (and presumably the 20-60 min OSD-build drops; runs #3-#5,#7 all flash-XIP builds). Months of "dead code perturbs sync" folklore explained.
- Oracle build still contains menu+watchdog code: now soaking as the confirming run with RS counter available. Production plan: make COPY_TO_RAM standard, un-park the root menu, commit option + patch code.

## 2026-06-12 — copy-to-RAM standardized; menu/watchdog un-parked; v0.7.1
- NEOPICO_COPY_TO_RAM committed and enabled for ALL CI firmware variants. Root menu + rate-based desync watchdog + RS counter committed (exonerated; the glitch was XIP). Default flag-off build verified byte-identical pre/post. Oracle soak (every formerly-fatal ingredient, from RAM): 2.5+ h clean and counting.

## 2026-06-12 — 2.1 S1 built: precomposed on the runtime-modes path
- pico_hdmi 2.1-dev (295f3dc..): precomposed machinery ported into video_output_rt.c (per-mode header ring, probe-discovered DI offsets for active AND blanking layouts, ISR island patching, pointer callback + native pixel mode, ring invalidation on apply_mode). Precomposed API factored into shared header video_output_precomposed.h (RT builds use video_output_rt.h, which lacked it). Scratch diet: ISR never calls line builders under precomposed (static nulls during ring build); builders demoted from scratch.
- build-s1 = RT lib + PRECOMPOSED + OSD + SELFTEST + ROOT_MENU + COPY_TO_RAM: links clean, scratch_x 0x7f0 (16 B under the boundary — THIN, trim before adding anything), 420 KB RAM total.
- This config is the architecture of record if it validates: tiny ISR + RAM execution + runtime-modes lib. Stages S2 (swap-free DMA), S3 (240p/720p pre-expanded rings), S4 (mode switching + Resolution menu) follow per pico_hdmi DESIGN-2.1.md.

2026-06-12 23:43 -03 - Status check: read STATE.md/SCRATCHBOOK.md and git status only. Current state remains 720p S3 precomposed+dynamic-genlock build on bench; next action is second-display cable-swap test before more code. Working tree is dirty in app and pico_hdmi submodule; nothing committed.

## 2026-06-14 — TRIPLE_ASM on the survivable S1 config (build-asm-s1, NOT flashed)
- User wanted the asm variant with the best soak-survival odds. Chose S1 (run #9 architecture of record, RT + PRECOMPOSED + OSD + SELFTEST + ROOT_MENU + COPY_TO_RAM, 480p), NOT any 720p-S3 build (runs #10-21 all had gray-flash/jolt).
- `build-asm-s1/` configured fresh: USE_NONRT_HDMI=OFF, PRECOMPOSED_HDMI=ON, COPY_TO_RAM=ON, ENABLE_OSD=ON, ENABLE_SELFTEST=ON, OSD_ROOT_MENU=ON, TRIPLE_ASM=ON. Links clean.
- CMake guard `CMakeLists.txt:309` forbids PRECOMPOSED + REBOOT_MODE_SWITCH (S4 unwired) → root menu is Self-Test-only; no Resolution entry. So at 480p the 2x is hardware; the 3x(720p)/4x(240p) asm kernels are NOT exercised on the live path — only by the boot self-test. This soak proves: asm links/boots, self-test ASM:OK on real silicon, presence is sync-neutral on the proven config. Live 3x stress needs a follow-up 720p build.
- Verified: kernels in .scratch_y (RAM, 90B/38B); scale_selftest + g_scale_asm_selftest_ok + fast_osd_puts_color present (ASM:OK/BAD renders); scratch_x 0x63c (<0x800, 452B headroom). UF2: build-asm-s1/src/neopico_hd.uf2.
- The earlier build-720p-s3 dir (cache mtime 17:17) had TRIPLE_ASM=ON but a stripped config (VIDEO_720P/PRECOMP/OSD/GENLOCK all OFF) — not a valid test vehicle; left untouched.
- NEXT: flash build-asm-s1 UF2, confirm ASM:OK on the Self Test screen, soak for sync stability.

## 2026-06-14 — build-asm-s1 FLASHED + Morph4K signal lead
- Flashed build-asm-s1/src/neopico_hd.uf2 (pi flash, device rebooted to app mode OK). Pending: confirm ASM:OK on Self Test row 12, then soak.
- DIAGNOSTIC LEAD (user report): RT/precomposed builds display fine on RT4K and direct-to-monitor, but misbehave on the Morph4K. The OLD 720p "non-RT" build worked fine on Morph4K. So the Morph4K is likely NOT at fault — the RT/precomposed signal has some marginal trait (suspect: Data Island layout, blanking-region DI, sync timing/polarity, or ACR) that the Morph4K input is stricter about than RT4K/monitor. Old non-RT path didn't have it.
- ACTION for asm soak: run on RT4K or direct monitor to avoid conflating the Morph4K signal issue with asm behavior.
- Future: to convict the RT-vs-nonRT signal delta on Morph4K, A/B the non-RT 720p build vs an RT build on the SAME Morph4K input; diff DI/blanking/sync between the two library paths.

## 2026-06-14 — build-720p-asm FLASHED (live 3x asm kernel)
- Config: compile-time NEOPICO_VIDEO_720P=ON + USE_NONRT_HDMI=ON + TRIPLE_ASM=ON (= CI "720p non-RT" known-good path + asm). Fresh dir build-720p-asm/. Flashed OK (rebooted to app mode).
- WHY this is the real asm test: the compile-time VIDEO_720P precomposed path calls video_pipeline_triple_pixels_fast LIVE at video_pipeline.c:322 & :326 (p720 ring prep, per line, every frame). The S1/480p build only ran the kernel in the boot self-test. This drives the 3x asm kernel under real load.
- Verified: triple kernel = asm version in .scratch_y (0x5a/90B); scale_selftest + g_scale_asm_selftest_ok present (TRIPLE_ASM=1). VIDEO_720P:BOOL=ON in cache.
- No OSD on this config (matches known-good CI build) -> no ASM:OK readout; visual correctness at 720p + the boot self-test are the proof. This is also the path the user says works on Morph4K (unlike RT/precomposed).
- WATCH: 3x-expanded geometry/colors correct (no shimmer/tear/wrong-pixel), sync stability over soak.

## 2026-06-14 — ASM EXONERATED on 720p; current-tree non-RT 720p is REGRESSED
- build-720p-asm (VIDEO_720P + non-RT + TRIPLE_ASM) HARD-CRASHED (USB dead/unresettable = HardFault, not an HSTX underrun).
- CONTROL build-720p-noasm (same config, TRIPLE_ASM=OFF) -> SAME hard crash. => asm is NOT the cause.
- CONCLUSION: the current dirty tree's COMPILE-TIME 720p path (NEOPICO_VIDEO_720P + USE_NONRT_HDMI) hard-crashes on its own. User's "old 720p non-rt works" was an OLDER commit; the path rotted during RT/precomposed (S1/S3) development. CI only proves it COMPILES, not that it runs.
- Open: this is a separate 720p regression hunt (NOT asm). The live 3x kernel call sites (video_pipeline.c:322/326) are only reachable on this broken compile-time path OR the RT reboot-switch path (line 933, the S3 build w/ its own jolt+Morph4K issues).
- Recovery: reflashed build-asm-s1 (480p RT precomposed) = working path; also has the ASM:OK self-test readout (kernel correctness still to be visually confirmed).
- NEXT options: (a) confirm ASM:OK on S1 to fully clear the kernels; (b) if live-3x soak desired, either bisect the non-RT 720p crash or test the RT/S3 720p path; (c) shelve asm (validated harmless) until a working 720p exists.

## 2026-06-14 — 720p hard-crash: likely PCB REVISION, not code
- User hypothesis: the 720p non-RT hard crash is the OLD PCB revision on the bench, not a code regression. The board that ran 720p when it was first introduced was given away; current bench board is a different OLD revision that may never have run 720p.
- Fits: 720p = sys_clk 372 MHz @ 1.30V, stresses power/SI; an old rev would HardFault rather than no-sync. Matches prior STATE caveat ("older PCB revision could interfere").
- Plan: user building a NEW board on the latest revision to retest 720p (incl. the live 3x asm kernel).
- Cheap disambiguation (optional): flash the ORIGINAL old 720p UF2 onto this board — also crashes => PCB convicted; runs => code regressed (bisect). Skipped unless the old UF2 is available.
- ASM STATUS: validated harmless (crash identical w/ and w/o asm). Live-3x soak deferred until working 720p hardware exists.

## 2026-06-14 — ROOT CAUSE: 720p hard-crash = XIP flash corruption at 372 MHz overclock
- build-720p-ram (VIDEO_720P + non-RT + COPY_TO_RAM=ON, no asm) -> 720p WORKS. Convicts XIP-flash-at-overclock as the crash cause.
- Mechanism: main.c:282-284 raises VREG 1.30V then set_sys_clock_khz(372000) but NOTHING re-tunes the QSPI/QMI flash divider. set_sys_clock_khz does not touch it. So flash clock scales 3x with sys_clk; at 372 MHz the XIP reads exceed the WeAct module flash chip's tolerance -> corrupt fetch -> instant HardFault (USB dies, unresettable). 480p (126 MHz, main.c:103) stays in spec -> fine.
- "Worked yesterday then both boards failed today, even old fw" = zero-margin XIP design tipping over (reflow/solder/thermal/aging nudge). NOT a damaged module: copy_to_ram proves the silicon is fine.
- FIX (durable): 720p must run from RAM (COPY_TO_RAM) OR properly re-tune the QMI clock divider at 372 MHz so flash stays <=~100-133 MHz (better: fixes XIP builds too). Note project already standardized COPY_TO_RAM for RT/precomposed variants; the compile-time NEOPICO_VIDEO_720P path had it OFF -> that's the gap.
- ASM now testable: build-720p-ram-asm (VIDEO_720P + non-RT + COPY_TO_RAM + TRIPLE_ASM) flashed. 3x kernel in .scratch_y; runs LIVE per-line at 720p from RAM. This is the original goal: live 3x asm soak vehicle.
- ACTION items for repo (NOT committed): (a) make compile-time 720p default COPY_TO_RAM (or add QMI divider tune); (b) consider erroring/ warning if VIDEO_720P && !COPY_TO_RAM.

## 2026-06-14 — Morph4K "squared" at 720p = sink aspect config, NOT firmware
- At 720p the picture is squared on Morph4K but CORRECT on RT4K and direct monitor => signal is fine; Morph-side issue.
- Firmware emits spec-correct 16:9: VIC=4, AVI InfoFrame byte2=0x28 (M=16:9, AFD=8 "same as frame"; hstx_packet.c:238). The 1280x720 frame is a true 16:9 raster with the 4:3 Neo Geo content pillarboxed (960px wide = 3x320, centered, 160px bars/side).
- Mechanism: Morph's restored "original settings" are a retro 4:3-source profile. Fed a real 16:9 720p frame, it squishes horizontally to its 4:3 expectation => game looks squared. (User confirmed hunch: "treating the signal as if it was 4:3".)
- FIX (sink-side, no firmware): set Morph aspect for this input to 16:9 / Source / Full (pass-through), not the 4:3 default.
- Rejected firmware lever: AFD 0x28->0x29 (4:3-center) would risk changing RT4K/monitor (currently correct) to fix one mis-configured sink. Not worth it; keep 16:9 signal.

## 2026-06-14 — 720p XIP-crash fix LANDED (copy_to_ram everywhere; QMI retune dropped)
- Decision: "RAM is the future for this project" (user). The COPY_TO_RAM path is THE fix; the QMI flash-timing retune (XIP-only) was built, validated to compile as a RAM function, then REMOVED as dead weight.
- neopico: src/CMakeLists.txt now FORCES NEOPICO_COPY_TO_RAM=ON whenever NEOPICO_VIDEO_720P or NEOPICO_EXP_REBOOT_MODE_SWITCH_720P is set (STATUS message on force). Verified: bare -DNEOPICO_VIDEO_720P=ON now auto-enables copy_to_ram; build links clean; main.c left untouched (the only neopico change is the CMake guard).
- lib/pico_hdmi examples: bouncing_box_rt (always 720p) and bouncing_box (under BOUNCING_BOX_720P) now call pico_set_binary_type(... copy_to_ram). Verified bouncing_box_rt: main now in RAM (0x2000...), links clean. Example main.c files reverted to original (no QMI code).
- QMI retune reference (if ever needed for a flash consumer): RXDELAY 1->3, keep CLKDIV=4 (93 MHz flash @ 372 MHz), via hw_write_masked(&qmi_hw->m[0].timing, ...) in a __no_inline_not_in_flash_func, called right after set_sys_clock_khz(372000). PicoDVI vista/main.c is the canonical pattern.
- NOT committed (house rule). lib/pico_hdmi is the hand-synced submodule -> mirror examples to ~/Projects/pico_hdmi.

## 2026-06-14 — pico_hdmi pushed upstream (origin/2.1-dev created)
- ~/Projects/pico_hdmi is the canonical dev tree (branch 2.1-dev). origin had only main + 2.0-beta; 2.1-dev had NEVER been pushed. Published it (13 commits) to github.com/fliperama86/pico_hdmi.
- Two new commits added before push: d54a4f0 (genlock: classify stretched-vtotal lines as blanking, not active video) + 2b43082 (examples: 720p copy_to_ram). Junk excluded (ANGETS.md typo, dist-2.0-beta/).
- DRIFT FLAGGED (not reconciled): neopico submodule lib/pico_hdmi is detached at old tip 3fe15b8 with its OWN uncommitted WIP (video_output_rt.c/.h differ from canonical) + the now-redundant example copy_to_ram edits. To track the pushed tip, advance submodule to 2b43082 and reconcile the submodule-local WIP. Left as-is pending user decision.

## 2026-06-14 — genlock WIP ported submodule->canonical and pushed (origin/2.1-dev 5e9e9e5)
- Ported the submodule's run #11-21 genlock work into ~/Projects/pico_hdmi and pushed: bottom-anchored active_video (slack lands at frame bottom; SUPERSEDES the earlier simple d54a4f0 active_video fix), elastic-blanking htrim subsystem (video_output_set_vblank_htrim_px + uniform precomposed-DI trim), perf probe (video_output_perf_probe_read FIFO-min/IRQ-gap). New public API in video_output_rt.h.
- pre-commit hooks (clang-format + clang-tidy) auto-normalized literal suffixes (u->U) + formatting on commit -> behavior-preserving. NOTE: this means canonical now differs COSMETICALLY from the submodule copy (submodule still lowercase-suffix). Exact parity would need syncing the hook-normalized files back to lib/pico_hdmi.
- Caveat recorded in commit: bench-validated through S3-720p runs; residual ~8-20s jolt still open (second-TV test pending).

## 2026-06-15 — OSD resumed: button-triggered instead of boot-open
- User wanted the OSD enabled via button press, not booting open.
- Root cause of boot-open: menu_diag_experiment.c ~541 unconditionally did genlock_screen_draw()+s_screen=GENLOCK+osd_show() under (OSD_ROOT_MENU && GENLOCK_DYNAMIC) as a soak aid. root_menu_buttons_tick() ALREADY opens the OSD from MENU_SCREEN_HIDDEN on MENU press; nothing else needed.
- Change (flag-gated, default OFF): new CMake option NEOPICO_OSD_BOOT_OPEN. Boot-open block now `#if NEOPICO_OSD_BOOT_OPEN && NEOPICO_OSD_ROOT_MENU && NEOPICO_EXP_GENLOCK_DYNAMIC`. Default (flag OFF) => OSD starts hidden (s_screen=MENU_SCREEN_HIDDEN=0, osd_visible=false), MENU button opens root menu. Flag ON preserves the soak-aid boot-open.
- Build build-720p-osdbtn (S3 genlock config: VIDEO_720P+PRECOMPOSED+GENLOCK_DYNAMIC+OSD+SELFTEST+ROOT_MENU+COPY_TO_RAM, BOOT_OPEN off). scratch_x 0x6d4 (<0x800). Flashed. Pending: confirm MENU press opens the menu on hardware.
- Not committed (house rule).

## 2026-06-15 — RT vs non-RT 720p are SIGNAL-IDENTICAL (Morph aspect is sink-side)
- A/B (single var = library path): build-720p-guardtest (non-RT) vs build-720p-rt-min (RT), both VIDEO_720P + copy_to_ram, nothing else.
- Result: rt-min = NO glitch + correct aspect on Morph; guardtest (non-RT) = no glitch but WRONG aspect on Morph.
- Measured both 720p render paths: IDENTICAL output. Content 960px (320x3) centered, 160px pillars each side, 1280x720, VIC=4, pixel_rep=0, pos sync, 74.4 MHz. AVI InfoFrame byte-identical (hstx_packet shared). Timing identical (1650x750).
- CONCLUSION: nothing in firmware differs in aspect between paths -> the Morph "wrong aspect on non-RT" is SINK-SIDE (its per-input/format auto-aspect), NOT a firmware bug. No non-RT firmware fix warranted. Caveat: DMA/DI command lists are built by different code; not 100% ruled out without an HDMI analyzer, but every standard param matches.
- PLAN: converge everything to RT mode eventually. Glitch is NOT the RT path itself (rt-min clean) -> it's precomposed and/or genlock and/or OSD. Hunting one variable at a time from rt-min.

## 2026-06-15 — RT 480p instability CONFIRMED = S2 DMA-swap hazard (not the selector)
- Resolution selector (REBOOT_MODE_SWITCH + 720P, RT, OSD, copy_to_ram) works; 240p/720p stable, 480p unstable. Selector static review CLEAN (scratch_x 0x614 < 0x800; ISR branches per-line; copy_to_ram neutralizes the old XIP layout ghost).
- Isolation: build-rt-480p (480p-only RT) = SAME instability as 480p-in-selector => inherent to RT-480p, NOT selector-specific.
- ROOT CAUSE (code-confirmed): video_output_rt.c:1086 `ch->al1_ctrl = posting_active_data ? dma_ctrl16 : dma_ctrl32` swaps DMA transfer size every IRQ. 480p posts active as 16-bit (HSTX does 2x expand) -> swaps 16<->32 every line. 720p/240p post 32-bit pre-expanded -> never swap. The per-IRQ al1_ctrl write racing a late/coalesced DMA IRQ = the documented S2 desync. Likely also the cause of the old precomposed 480p 20-30min soak failures.
- FIX = S2 (swap-free DMA). Options: (A) make 480p 32-bit pre-expanded like 720p/240p (app already double_pixels_fast; most contained, unifies all modes on the proven swap-free topology); (B) dedicated per-post DMA channels (STATE S2 rework). Lean (A). Library surgery on hot ISR -> scope before coding.
- Selector firmware itself is GOOD; 480p just rides the broken path. S2 unblocks 480p everywhere.

## 2026-06-15 — CORRECTION: 480p instability is ISR CYCLE BUDGET, not the DMA swap
- PRIOR ENTRY WRONG: the al1_ctrl 16/32 swap (video_output_rt.c:1086) is inside `#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES` AND `if(native_pixel_mode)` -> PRECOMPOSED/native ONLY. The selector is NOT precomposed, so that block isn't compiled. 480p there already uses the plain 32-bit copy model, no swap. Approach A (swap-free DMA) does NOT apply.
- REAL CAUSE (quantified): cycles per scanline-IRQ = sys_clk / line_rate.
  480p: 126MHz / 31.5kHz = ~4000 cyc/IRQ (h_total 800, v_total 525, pixel 25.2MHz, line-doubled).
  240p: 126MHz / 15.75kHz = ~8000 (h_total 1600, v_total 262).
  720p: 372MHz / 45kHz = ~8250 (h_total 1650, v_total 750).
  => 480p has HALF the per-line ISR budget; per-line work (DMA post + DI/audio tick + pixel-scale callback) occasionally overruns -> FIFO underrun -> desync. 240p/720p have ~2x headroom. Matches user: 480p unstable, 240p/720p stable.
- PROPOSED FIX = run 480p at higher sys_clk (~252 MHz) with HSTX divider doubled (clk_div 1->2 or csr_clkdiv 5->10) so pixel stays 25.2 MHz (identical picture), doubling ISR budget to ~8000. Needs vreg 1.30V + copy_to_ram (already forced in selector). Touches main.c (sys_clk per mode) + library 480p mode dividers. Build 480p@252 to CONFIRM + likely fix in one shot before committing to the approach.

## 2026-06-15 — OSD background blending question
- User asked whether SIMD/ASM tricks could make fast OSD bg <-> game-content pixel blending feasible.
- Relevant constraints re-read: OSD is Core 1 overlay during scanline doubling DMA ISR; HSTX 480p line timing is exactly 800 cycles; inner loop must avoid per-pixel branches and minimize memory access; Core 0 must stay capture-only.
- Initial answer direction: possible only in very constrained forms (mask/substitute or coarse/quantized blend LUTs), but true arbitrary alpha blending per OSD pixel is likely too expensive/risky in the ISR, especially 480p budget; any experiment should be compile-time flag-gated/off by default.

## 2026-06-15 — Perf: Tier 1 banked; command-list scanout investigated + PARKED
- Tier 1 (committed f8034c7 / neopico f8ef31e): perf-probe gated off, v_scanline modulo->compare, get_scanline_state boundary hoist. ~45-70 cyc/line off the ISR, all modes. The genuine free win.
- DEEP investigation of rp2350-doom's no-per-line-ISR command-list scanout (see ~/Projects/rp2350-doom/src/pico/hstx_cmdlist.c + INVESTIGATION_PROGRESS.md). Findings:
  - Root perf constraint = the PER-LINE ISR deadline (confirmed cross-project). doom eliminated it: 1 static DMA command list/frame, 2 chained channels (pixel CHAIN_TO command; command write-ring-4 into pixel al3), 1 IRQ/frame. Mode-agnostic.
  - Command-list model absorbs FOR FREE: two-phase active line, the al1_ctrl 16/32 swap (baked into per-slot ctrl), blanking variety, v_scanline wrap.
  - 480p horizontal 2x = 16-bit DMA bus-replication + HSTX expander -> ZERO software scaling, framebuffer stays 320-wide. (We have this as native_pixel_mode.)
  - RAM caps: 480p native frame ~150KB fits (fully static list). 240p(4x)/720p(3x) full pre-expanded frame = 614KB+ -> DOESN'T fit 520KB; needs ring + periodic (per-segment, ~few/frame) pointer refresh, not per-line.
  - Hard problems: (1) AUDIO Data Islands = doom's UNSOLVED part ("stable video, mute audio" on real sinks via cmdlist) -> the make-or-break risk; (2) genlock vtotal step vs static list (h-trim compatible, vtotal needs dual-list relink); (3) live-capture full-frame buffer.
- The cheapest standalone win beyond Tier 1 = 480p hardware-2x (native pointer + 16-bit DMA) to kill the ~1000 cyc/line scale loop, BUT in the per-line model it reintroduces the al1_ctrl swap = the S2 desync race that dropped precomposed at 20-30 min. Clean only inside the full command-list model.
- DECISION (user): not worth the risk. Tier 1 stays; 480p stays at 252 MHz OC (stable, harmless). Full command-list rewrite parked (audio-DI risk + rewrite cost). branch perf/doom-scanout deleted (was empty).

## 2026-06-15 — Cold-boot scratchy audio FIXED via auto-reboot (shipped, default ON)
- Symptom resurfaced: scratchy audio in loud games (SamSho, KOF98), not Metal Slug X. Same as project's early-days cold-boot scratchiness; manual reset fixes it. Game-dependence = loud voice content exposes a cold-boot STATE issue (MVS DAC settle / TV audio decoder latches DIs before TMDS lock), not content.
- Fix: NEOPICO_EXP_FIRST_BOOT_REBOOT (default ON, commit 5688054). main.c: if cold boot (take_reboot_mode_boot_request==false), call video_pipeline_request_reboot_mode(default) + spin -> one auto-reboot. Warm boot proceeds (scratch magic). Replicates the manual reset (warm HDMI re-lock).
- HW confirmed: CLEAN after the auto-reboot. Cost ~1-2s extra cold boot; warm/resolution reboots unaffected. Workaround, not root-cause fix.
- NOTE: this landed AFTER tag v0.8.0 (5688054 is post-tag on main). v0.8.0 release does NOT include it; would need a v0.8.1 to ship the audio fix in a release.

## 2026-06-15 — Flash-backed settings (resolution persists across power-off) — MERGED
- Done in a worktree (worktree-flash-settings), validated on HW, ff-merged to main (b61c782), worktree+branch torn down.
- src/settings.{c,h}: magic+version+CRC record in the LAST 4KB flash sector (16MB part; SETTINGS_FLASH_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE). Payload = resolution + reserved[31] for growth. settings_load (read XIP), settings_save (__no_inline_not_in_flash_func; CRC before XIP suspend; erase+program under save_and_disable_interrupts). Reused MarkI- mechanics, simplified to single-sector (no journal — writes are rare + controlled).
- Integration: request_reboot_mode() persists the new mode (blocking, before watchdog_reboot — brief ISR stall invisible since rebooting). Cold boot loads saved resolution as the boot target (warm reboots still use watchdog scratch). Was: resolution reset to 480p on every power-off.
- NEOPICO_SETTINGS_FLASH (default ON), links hardware_flash. copy_to_ram (forced by selector) makes the flash write safe (whole binary in RAM, XIP suspend stalls nothing).
- HW: resolution survives cold power-cycle. "BEAUTIFUL" per user.
- NOTE: post-tag v0.8.0 (b61c782 on main). Next release (v0.8.1?) would bundle this + the cold-boot audio fix (5688054).

## 2026-06-15 — Clarified fake OSD blend concept
- User asked if the proposed OSD blend means "faking" transparency by darkening the underlying game pixels.
- Clarification: yes; safest concept is dim game pixels in the OSD panel region, then draw opaque OSD text/colors on top, avoiding true arbitrary alpha blending in the HSTX scanline ISR.

## 2026-06-15 — Deferred OSD fake-blend experiment
- User liked the fake translucent OSD idea: darken game pixels under the OSD rectangle, then draw opaque OSD text/colors on top.
- Deferred until after the repo is converted into a "bare" one.
- Future test shape: compile-time feature flag, off by default; fixed dim factor only; no arbitrary alpha; keep Core 1 scanline ISR branchless/loop-split and easy to rollback.

## 2026-06-15 — Created translucent OSD worktree
- User decided to use normal Git worktrees, not a bare-repo model.
- Created sibling worktree `/Users/dudu/Projects/neogeo/neopico-hd-translucent-osd` on new branch `exp/translucent-osd` from `main` HEAD 46cd8f7.
- Initialized `lib/pico_hdmi` submodule in the new worktree at recorded gitlink f8034c7. No implementation code changed yet.
- Reminder for future experiment: fake translucent OSD = dim game pixels under OSD rectangle, then render opaque OSD text/colors; feature-flag off by default.

## 2026-06-15 — Resolution-confirm safety net (MERGED 9c293c9)
- TV-style "Keep this resolution?" 10s countdown after a res switch. MENU keeps, BACK/timeout reverts. Auto-recovers from picking a mode the display can't show (black screen -> timeout -> previous res). NEOPICO_OSD_RES_CONFIRM default ON.
- GOTCHA (cost a debug cycle): the pending state must ride watchdog scratch[3] ONLY. The SDK's watchdog_reboot(0,0,..) ZEROES scratch[4] (and uses [4..7] for its boot vector). The existing reboot mode already owns scratch[0..2]. So only scratch[3] survives a plain reboot -> packed marker+previous-mode+check into that one register. First attempt used scratch[3..5] -> scratch[4] clobbered -> take_pending failed -> no prompt.
- Order INVERSION (cost a debug cycle): originally persisted on CONFIRM (live flash erase, no reboot after) -> the interrupt-disabled erase stalled the HDMI ISR and dropped sync UNRECOVERABLY. Fixed: persist optimistically at SELECT (masked by that reboot); CONFIRM = pure dismiss (no flash); REVERT/timeout rolls flash back to previous + reboots. All flash writes now at reboot points.
- Edge case (accepted): power-cut DURING the countdown persists the unconfirmed res (no boot-completed flag). Rare; bulletproofing = a boot-completed flag cleared on confirm.
- Bonus: settings.c now includes pico.h not pico/platform.h (a linter reorder had broken clean builds since b61c782; this merge un-breaks main).

## 2026-06-15 — Reviewed 720p purple-glitch tracker skeptically
- User asked for opinion on `docs/720P_PURPLE_GLITCH.md` and warned to take it with a grain of salt.
- Read tracker + older `docs/720P_SAMSUNG_GAME_MODE_INVESTIGATION.md` + relevant 720p clock/HSTX/capture paths. No code/build/hardware commands run.
- Current read: strongest fact is output-side/global corruption if black bars + OSD are truly affected; that argues against captured pixel data/content and toward marginal TMDS eye disturbed by live capture workload.
- Caveats: many clean cases were short (~2 min), static-pattern-with-live-capture from older note conflicts with “capture workload alone,” FIFO probe only samples per-line, and exact mechanism (power/EMI vs HSTX/PLL jitter vs rare sub-line bus event) remains unproven without scope or longer A/B logs.
- Recommended next: establish 20–30 min baseline timestamps, repeat key clean controls, then scope 3V3/core rails + TMDS/clock while toggling capture/freezing; keep firmware experiments flag-gated/off by default.

## 2026-06-16 — 720p purple-glitch: capture-timing ruled out; labeled experimental
- Big correction this session: earlier "Core 0 capture activity / power-EMI" conclusion was WRONG. **test-pattern + FULL live capture = CLEAN** (same Core 0 load, no glitch) ⇒ the differentiator is **Core 1 rendering LIVE captured video**, not capture workload/power/clock.
- Added flag-gated `NEOPICO_DIAG_COUNTERS` (off by default): line-ring NOTWR/OVR, capture SYNCRST, in/out frame rates, dumped over USB-CDC. Across two clean windows (~160s + ~145s) of glitchy play with confirmed glitches: **ALL FLAT** ⇒ capture-timing/readiness/sync/handoff very likely NOT the cause (lines delivered on-time + ready through glitches).
- USB-CDC at 372MHz OC stalls streaming after ~2.5min (counters keep accumulating in firmware though). Don't peek mid-capture (abrupt host close wedges TinyUSB CDC TX).
- Localization (content-only vs global incl black bars/OSD) UNRESOLVED — flip-flopped on TV photos; even high-fps frame-steps conflicted. Don't conclude mechanism from photos.
- Open fork (counters can't see either): (a) wrong DATA in an on-time line (capture sampling: PCLK phase/RGB-bus) vs (b) output-side (scaler/HSTX). Next discriminator: solid high-contrast local band over live capture, frame-step a glitch.
- DECISION: label 720p "Experimental (3x)" in the resolution OSD; ship as experimental. Full tracker: docs/720P_PURPLE_GLITCH.md (+ older docs/720P_SAMSUNG_GAME_MODE_INVESTIGATION.md; should be merged).

## 2026-06-27 - Audio startup re-arm experiment
- User reported KOF98/Darksoft audio boots either clean or very scratchy, with no worsening over time. Interpreted as startup I2S framing/state issue, not drift or PLL problem.
- Implemented NEOPICO_EXP_AUDIO_STARTUP_REARM default ON: Core 1 audio state machine now does initial muted warmup, one-shot stop/restart of I2S PIO/DMA, second muted warmup, flushes capture/SRC/collect state, then unmutes. Build passed in build-audio-rearm-check.
- Noted unrelated KiCad hardware project/history dirt in git status after build check; do not treat as part of audio fix unless user confirms cleanup.
- Also verified rollback path: default build passed with startup re-arm ON, and build with -DNEOPICO_EXP_AUDIO_STARTUP_REARM=OFF passed.

## 2026-06-27 - Flashcart audio distortion after game select
- User tested standalone KOF98 vs Darksoft flashcart: menu audio is fine, distortion appears after picking a game. Updated hypothesis: flashcart/console reset or audio-clock disturbance occurs AFTER the one-shot boot re-arm, so startup-only rearm is too early for this path. Next likely fix: trigger an audio-only re-arm on MVS video/sync reacquire or provide a manual/OSD audio reset to confirm.

## 2026-06-27 - J5 as flashcart reset-detect input
- Checked KiCad/read-only: J5 AUX is a 2x04 header exposing A1/RP2350 GPIO0-GPIO7 (pads 1-8). Firmware currently uses video GP27-45, audio GP22-24, OSD GP25-26, so J5 GPIO0-7 are free candidates for an MVS/flashcart reset detect input. Prefer GP4-GP7 to avoid future UART habits; add protection/series or buffer for 5V/reset-line safety.

## 2026-06-27 - Manual audio-only re-arm test path
- Implemented NEOPICO_EXP_AUDIO_MANUAL_REARM default ON for diagnosis: when audio is running, holding MENU+BACK (GP25+GP26) for ~0.5s mutes audio, stop/restarts I2S PIO/DMA, warms muted for ~0.5s, flushes capture/SRC/collect state, then unmutes. Requires button release before retrigger.
- Built default and -DNEOPICO_EXP_AUDIO_MANUAL_REARM=OFF successfully. Intended test: after Darksoft game select causes distortion, hold MENU+BACK; if audio clears, flashcart reset/audio-clock disturbance hypothesis is confirmed.

## 2026-06-27 - Flashed manual audio re-arm test build
- Flashed build-audio-manual-test/src/neopico_hd.uf2 successfully with pi flash. Build includes NEOPICO_EXP_AUDIO_STARTUP_REARM=ON and NEOPICO_EXP_AUDIO_MANUAL_REARM=ON. Test action: after Darksoft game-select distortion, hold MENU+BACK for ~0.5s; audio mutes/rearms/rewarms then unmutes.

## 2026-06-27 - Manual audio re-arm confirmed; BACK-only preferred
- HW test result: manual audio-only re-arm fixed Darksoft post-game-select distorted audio. Confirms the capture can become mis-framed after flashcart/console reset or audio-clock disturbance.
- User preference for next build: use BACK-only instead of MENU+BACK. Updated NEOPICO_EXP_AUDIO_MANUAL_REARM trigger to hold BACK for ~0.5s, built build-audio-back-test/src/neopico_hd.uf2. Await explicit ready before flashing.

## 2026-06-27 - Proper Darksoft audio fix: auto re-arm on video reacquire
- Implemented NEOPICO_EXP_AUDIO_REARM_ON_VIDEO_REACQUIRE default ON. Core 0 video capture marks audio re-arm pending after MVS no-signal timeout/sync reset; on next VSYNC reacquire it calls audio_subsystem_request_rearm(). Core 1 consumes the request in audio_subsystem_background_task(), mutes, stop/restarts I2S PIO/DMA, rewarmes muted for ~0.5s, flushes capture/SRC/collect state, and unmutes.
- Manual BACK re-arm kept only as NEOPICO_EXP_AUDIO_MANUAL_REARM default OFF. Default, auto-off, and startup+auto-off builds passed. Flash candidate: build-audio-auto-fix/src/neopico_hd.uf2.

## 2026-06-27 - Flashed auto audio re-arm fix
- Flashed build-audio-auto-fix/src/neopico_hd.uf2 successfully with pi flash. This build has automatic audio re-arm on MVS video/sync reacquire; manual BACK re-arm is compiled OFF by default. Test target: Darksoft menu -> select game -> verify game audio no longer distorted without manual reset.

## 2026-06-27 - Auto audio re-arm HW result
- User tested Darksoft menu -> game select with auto re-arm build and reports it seems fixed. Likely release-worthy after a bit more soak. Root cause now treated as MVS/flashcart reset or sync/audio-clock disturbance causing I2S capture mis-framing; automatic video-reacquire audio re-arm clears it.

## 2026-06-27 - Auto video-reacquire audio fix insufficient
- User reports audio issue still persists after auto re-arm-on-video-reacquire build. Conclusion: Darksoft game select likely disturbs/resets the audio/I2S domain without causing a long enough MVS video no-signal timeout, so the Core 0 video reacquire trigger is insufficient. Manual audio re-arm still confirmed as effective, so fix needs a better trigger: hardware reset detect via J5/GPIO, audio-domain clock-gap watchdog, or keep BACK manual fallback.

## 2026-06-27 - Audio fix constraints: no manual, avoid new wiring
- User clarified manual audio reset is out of question for a release. New reset wiring via J5 is possible but not ideal. Need explore software-only automatic triggers beyond video no-signal/reacquire, because that trigger missed Darksoft game-select distortion while manual re-arm remains effective.

## 2026-06-27 - Software audio alternative: per-frame I2S WS re-sync
- Implemented NEOPICO_EXP_AUDIO_FRAME_RESYNC default ON. Added new PIO program i2s_capture_frame_resync that waits for WS high->low at the start of every stereo frame before capturing RIGHT then LEFT. Goal: if Darksoft briefly glitches BCK/WS during game launch, capture drops/reacquires a frame instead of staying permanently mis-framed.
- Selected new PIO program for MV1C path only; PCM1802 path unchanged. Default build, -DNEOPICO_EXP_AUDIO_FRAME_RESYNC=OFF rollback build, and -DNEOPICO_AUDIO_PCM1802=ON build all passed. Flash candidate: build-audio-frame-resync/src/neopico_hd.uf2.

## 2026-06-27 - ADV7513 via RP2350 PIO feasibility note
- User asked whether RP2350 PIO could drive ADV7513 at 720p60. Current conclusion: possible only with a narrow, optimized low-pin-count ADV7513 input mode, preferably YCbCr 4:2:2 DDR or 2x-clock on 8/12 data pins, not practical as straightforward 24-bit RGB SDR at 74.25 MHz because DMA bandwidth/pin count/Core work is too high. Need external pixel-clock quality and ADV7513 I2C init, plus RGB->YCbCr/packing if using 4:2:2.

## 2026-06-27 - Added TDP158 KiCad library parts
- Added project-local TDP158 symbol to hardware/neopico-hd/neopico-hd.kicad_sym and footprint Library.pretty/TDP158_RSB_WQFN-40-1EP_5x5mm_P0.4mm_EP3.15x3.15mm.kicad_mod. Footprint uses TI RSB0040E land pattern values from TDP158 datasheet: 5x5mm WQFN-40, 0.4mm pitch, 40x 0.2x0.6mm perimeter pads, 3.15x3.15mm exposed pad with 4 paste apertures. Symbol maps TDP158 pins incl TMDS lanes, DDC/HPD/control, VCC/VDD/GND/EP.
- Validated with KiCad CLI SVG export for symbol and footprint. Did not wire redriver into schematic yet.

## 2026-06-27 - TDP158 library correction: pico-retrodigital
- User clarified the TDP158 symbol/footprint should be added to /Users/dudu/Projects/pico-retrodigital, not only NeoPico. Added TDP158 symbol to hardware/pico-retrodigital/pico-retrodigital.kicad_sym and footprint to hardware/pico-retrodigital/pico-retrodigital.pretty/. Validated both with KiCad CLI SVG export. Did not wire into schematic. Earlier NeoPico library addition remains unless explicitly cleaned.

## 2026-06-27 - Firmware audio fix committed and pushed
- Committed firmware-only changes as b8cdba7 (audio: recover I2S capture after flashcart resets) and pushed main to origin. Commit intentionally excluded SCRATCHBOOK, hardware/KiCad edits, TDP158 library work, and build artifacts. Build validation before commit: cmake --build build-audio-frame-resync --target neopico_hd -j8 passed; pre-commit hooks passed after pointing build/ to the validated build dir.

## 2026-06-27 - DARK/SHADOW RAM budget concern
- True RGB15+DARK+SHADOW conversion as a flat uint16 LUT needs 131072 entries = 256KB. With current large buffers (line_ring about 160KB, OSD framebuffer about 56KB, I2S DMA about 16KB, optional 720p ring about 40KB) it is likely too tight or impossible in the current full-feature RP2350 build, especially with copy-to-RAM overclock builds. Prefer not making full 256KB DARK/SHADOW LUT default without a map-file proof or reducing other buffers/features.
- 480p-only RAM estimate for true DARK/SHADOW: removing the 720p ring saves about 40KB, but 256KB LUT + 160KB line ring + 56KB OSD + 16KB audio DMA + misc/stacks/code-in-RAM still leaves little/no safe headroom. 480p might fit only with feature reductions and map-file validation, not as a safe default.

## 2026-06-27 - User wants pragmatic DARK/SHADOW experiment
- User pushed back on over-defensive RAM concern. Proceeding pragmatically: try true DARK/SHADOW behind a compile-time flag, target 480p first, use build/map to decide fit rather than rejecting upfront. No hardware flashing involved.
- Implemented true DARK/SHADOW as a 3-state LUT instead of brute-force 17-bit LUT: normal, DARK-only, SHADOW with DARK forced. Size is 3*32768*2 = 192KB. Updated docs/CMake wording.
- Build results: default rollback build passes. Fixed RT 480p + OSD + true LUT still overflows RAM by 23460 bytes. Non-RT fixed 480p + OSD + true LUT builds. RT fixed 480p + true LUT builds when OSD is disabled. No hardware flashed.
- clang-format was not available in the shell (`command not found`), so the DARK/SHADOW C edit was left manually formatted. Temporary build directories were removed after validation.
- Rebuilt flash candidate build-dark-shadow-480p-nonrt/src/neopico_hd.uf2: fixed 480p non-RT HDMI, OSD on, true DARK/SHADOW enabled. Build passed. Waiting for explicit hardware-ready ack before running pi flash.
- Flashed build-dark-shadow-480p-nonrt/src/neopico_hd.uf2 successfully with pi flash. Candidate is fixed 480p non-RT HDMI, OSD on, true DARK/SHADOW enabled. Tool rebooted RP2350 from USB serial to BOOTSEL and back to app mode.

## 2026-06-27 - True DARK/SHADOW non-RT sync drop
- HW result: after flashing fixed 480p non-RT HDMI + OSD + true DARK/SHADOW, user reports sync drops. Treat non-RT 480p output path as likely regression source. Next quick isolation: flash RT 480p true DARK/SHADOW with OSD disabled, since that build fits and keeps the known RT output path.

## 2026-06-27 - Correction: uncontrolled non-RT variable
- User corrected a serious workflow mistake: flashing the non-RT 480p build introduced an uncontrolled output-path variable and invalidated the DARK/SHADOW experiment. Do not change HDMI path, OSD, resolution selector, or other build axes unless the user explicitly requests it. Only change the requested variable. Treat the non-RT sync-drop result as invalid for DARK/SHADOW conclusions.

## 2026-06-27 - DARK/SHADOW LUT quantization question
- User asked whether the DARK/SHADOW LUT can be quantized. Need answer/design only unless explicitly told to implement. Keep experiment controlled: do not change HDMI path/OSD/resolution selector to make RAM fit.
- Implemented quantized true DARK/SHADOW LUT while preserving the normal build path: exact 32K normal RGB LUT plus two 16K effect LUTs (DARK and SHADOW-with-DARK), total 128KB. Quantization drops only corrected blue LSB for effect pixels. Built default config with only NEOPICO_ENABLE_DARK_SHADOW=ON in build-dark-shadow-quantized; build passed. Waiting for explicit ready before flashing build-dark-shadow-quantized/src/neopico_hd.uf2.
- Flashed build-dark-shadow-quantized/src/neopico_hd.uf2 successfully with pi flash. This is the controlled RT/default path with only NEOPICO_ENABLE_DARK_SHADOW=ON and quantized 128KB effect LUT. pi flash needed one retry to enter BOOTSEL, then loaded and rebooted to application mode.
- Committed and pushed quantized DARK/SHADOW implementation as 81e95ab (video: add quantized DARK/SHADOW LUT) to origin/main. Scope intentionally limited to src/video/video_capture.c, src/CMakeLists.txt, and DARK/SHADOW docs. Feature remains behind NEOPICO_ENABLE_DARK_SHADOW (default OFF). Excluded SCRATCHBOOK, hardware/KiCad dirt, and build artifacts. Validation: default build passed in build/; NEOPICO_ENABLE_DARK_SHADOW=ON build passed in build-dark-shadow-quantized; pre-commit hooks passed.

## 2026-06-27 - Capture target file split preference
- User corrected architecture detail: keep MVS and SNES target files separated as much as possible. Do not put SNES definitions in `mvs_pins.h`. Use target-specific files plus neutral selector/config headers for shared code.

## 2026-06-27 - NeoPico capture-target split initial implementation
- Implemented initial NeoPico-side target split with separate capture files: `video_capture_mvs.c/.pio` and `video_capture_snes.c/.pio` selected by `-DNEOPICO_CAPTURE_TARGET=MVS|SNES`.
- Added neutral `capture_profile.h` and `capture_pins.h`; added target-specific `snes_pins.h`; removed SNES definitions from `mvs_pins.h` per user correction.
- Shared HDMI/audio/output path remains NeoPico RT path. Verified compile commands for both default MVS and SNES use `video_output_rt.c` with `PICO_HDMI_480P_HSTX_CLK_DIV=2`.
- SNES capture PIO divides sysclk back to 126 MHz timing when NeoPico RT 480p runs sysclk at 252 MHz.
- Build validation passed: default `build` MVS target and separate SNES validation build with `-DNEOPICO_CAPTURE_TARGET=SNES`. Removed temporary SNES build dir afterward.

## 2026-06-27 - Flashed NeoPico SNES capture target
- User requested build and flash. Built `build-snes-capture` with `-DNEOPICO_CAPTURE_TARGET=SNES`; target `neopico_hd` passed.
- Flashed `build-snes-capture/src/neopico_hd.uf2` successfully with `pi flash`. Tool rebooted RP2350 from USB serial to BOOTSEL, loaded firmware, and rebooted to application mode.
- Firmware is NeoPico shared RT HDMI/audio/output path with SNES/SuperPico capture backend selected.

## 2026-06-27 - SNES target first HW result
- User reports SNES target firmware works but has two issues: video glitches quite a lot at startup then settles; audio is busted and sounds like highly compressed audio.
- Immediate hypothesis: audio path kept NeoPico/MVS SRC default DROP mode, which is appropriate for 55.5k->48k decimation but wrong for SNES ~32k->48k upsampling. Need select LINEAR SRC for SNES. Startup video likely needs SNES capture warmup/valid-frame gating or later capture trigger after H/V signals settle.

## 2026-06-27 - SNES startup glitch and audio-compression fixes
- User reports SNES target works but startup video glitches heavily before settling, and audio sounds highly compressed.
- Applied SNES-target audio fix already in progress: use LINEAR SRC for SNES 32.04k -> 48k upsampling instead of MVS DROP decimation, and enlarge audio process output buffer to handle expansion.
- Added SNES-only capture warmup gate, default 60 frames, before starting line_ring capture/PIO IRQ after power-up or firmware boot. Goal: hide unstable early H/V/PCLK capture from HDMI output.
- Build validation passed: SNES target in build-snes-capture and default MVS target in build both build `neopico_hd` successfully. No flash run on this report turn.

## 2026-06-27 - Flashed SNES audio/SRC and startup warmup fix
- User requested flash. Flashed `build-snes-capture/src/neopico_hd.uf2` successfully with `pi flash`.
- Build includes SNES target, LINEAR SRC with larger output process buffer for 32.04k -> 48k audio upsampling, and 60-frame SNES capture warmup gate for startup video glitches.

## 2026-06-27 - SNES target fixes confirmed and staged
- User reports flashed SNES audio/SRC and startup warmup build is good.
- Staging requested. Stage only firmware/source changes for NeoPico SNES capture target, audio profile/SRC fixes, and MVS file rename. Exclude SCRATCHBOOK, build artifacts, and unrelated hardware/KiCad dirt.

## 2026-06-27 - SNES DSP audio wiring reminder
- For NeoPico SNES target audio, firmware expects DSP44 DATA/SDATA -> GP22, DSP43 LRCK/WS -> GP23, DSP42 BCK/BCLK -> GP24, plus common GND. DCK/DSP78 and RESET/DSP47 are needed by SPDIF mod boards, but not by NeoPico audio capture unless reusing the mod board as a pass-through reference.
- Lectronz TRC V2 page warns SHVC install uses 5 scraped vias in the sound module, keep wires short, and go by DSP/APU pin numbers because connector/non-connector board ordering can differ.

## 2026-06-27 - SHVC blue solder-mask scraping concern
- User clarified SNES is SHVC and asked whether there are reports of issues after scraping blue solder mask on SHVC sound module vias for the Lectronz/TRC digital audio mod. Web check found the Lectronz install explicitly instructs gently scraping 5 SHVC sound-module vias and tin/wick them; no specific public report found that scraping itself causes systematic issues. Known risk is ordinary via/pad damage, solder bridges, weak joints, exposed copper/short risk, plus SHVC cap leakage and wiring/noise/DAC-compatibility issues.

## 2026-06-27 - SHVC audio-related capacitor guidance
- User asked about SFC/SHVC audio-related caps. Researched current cap lists: SHVC-CPU-01 has C50/C51 47uF 10V inside SHVC-SOUND module with <8mm height constraint; these are the first target for analog audio-module maintenance. Full SHVC list also includes C57-C67, with C67 main bulk and optional 470uF regulator-output cap in kits.
- Important distinction: NeoPico digital audio capture taps S-DSP DATA/WS/BCK before analog DAC/output caps, so analog caps usually will not fix digital-capture artifacts unless power/filtering is bad enough to disturb the DSP/clock.

## 2026-06-27 - SHVC sound-module cap values
- User asked cap values. Answered: SHVC audio module caps C50/C51 are 47uF 10V radial, about 6.3mm diameter, 5mm board pitch, must fit under <8mm shield height. 16V replacements are electrically fine if same capacitance and physically fit. Full SHVC-CPU-01 recap values: C57-C60 100uF 6V, C61/C65/C66 10uF 16V, C62 2.2uF 50V, C63/C64 33uF 25V, C67 1000uF 25V.

## 2026-06-27 - Full SHVC-CPU-01 capacitor list shared
- User asked to list all SHVC/SFC capacitor values. Provided SHVC-CPU-01 electrolytic list: C50/C51 47uF 10V radial in SHVC-SOUND, C57-C60 100uF 6V SMT, C61/C65/C66 10uF 16V SMT, C62 2.2uF 50V SMT, C63/C64 33uF 25V SMT, C67 1000uF 25V radial. Noted C50/C51 height constraint under shield.

## 2026-06-30 - Branch check
- User asked whether repo is already on main. Confirmed current branch is main tracking origin/main. Worktree has pending SCRATCHBOOK, hardware/KiCad dirt, build-snes-capture, and staged/modified SNES capture firmware changes.

## 2026-07-06 - MV1B install no game picture report
- User reports external MV1B install: HDMI/OSD path works perfectly, but no game picture. Installer measured signals on Pico board and says they look fine. Current hypothesis: not HDMI/output; likely MVS capture cannot parse MV1B sync/PCLK phase/source. MV1B uses different video ASIC set (NEO-GRC2-F) than MV1C assumptions, but Neo Geo MVS video clock should still be 6MHz from 24MHz/4. Most likely differences to check: CSYNC polarity/source or missing composite-vsync/equalization at the chosen tap, PCLK edge/phase, or RGB bit/tap order.
- MV1B installer measured 12MHz on supposed PCLK. User suspects they tapped system/12MHz clock rather than the 6MHz video pixel clock. This explains no captured game picture while OSD/HDMI works. Firmware expects ~6MHz PCLK and about 384 clocks per line.
- Neo Geo PCLK answer: the desired capture clock is the 6MHz/6MB video clock, i.e. 24MHz master divided by 4. NeoGeoDev says classic cartridge systems use NEO-D0 for 24/2, 24/4, 24/8 clocks; Signals lists 12M as 24/2 and 6MB as 24/4 inverted. MV1B uses NEO-GRC2 late video ASIC, so exact physical pin should be verified on MV1B pinout/board scans, but installer should search for 6M/6MB, not 12M.

## 2026-07-06 - MV1B PCLK generation clarification
- User asked where PCLK is generated. Answered: capture PCLK should be Neo Geo video 6M/6MB, the 24MHz master divided by 4. Classic docs attribute 24/2, 24/4, 24/8 clock generation to NEO-D0; MV1B uses NEO-GRC2-F late video ASIC, so find the board's 6M/6MB net or pinout rather than using measured 12M.
- Exact MV1B PCLK/6MB tap found in NeoGeoDev NEO-GRC2 pinout: NEO-GRC2 pin 2, labelled 6MB. In the pinout image it is on the bottom edge, second pin from the lower-left corner, between pin 1 NC and pin 3 GND. NEO-GRC2 also has 24M on pin 190.
- Verified MV1B/NEO-GRC2 confidence: NeoGeoDev MV1B page lists NEO-GRC2-F on MV1B1 main board; MVS board types lists MV1B and MV1B1 video chipset as NEO-GRC2. Still advise verifying the actual board photo/chip marking before giving install point, because installs may be MV1A/MV1C or odd revisions/modded boards.
- MV1B sync clarification: NeoPico MVS firmware expects composite sync/CSYNC on GP27, active-low, not separate H/V or GRC2 H/EVEN timing pins. JAMMA/MVS edge pin P is Video sync and can be used as the source if properly level shifted/conditioned; firmware derives VSYNC from the composite sync pulse pattern.
- MV1B sync tap warning: if U6 is a 74LS273, pin 1 is the common active-low CLR/MR input, not CSYNC. U6/U7 LS273s are color latches on MV1B-class boards; pin 1 may be RGB blanking/latch clear and can look sync-like, but it is not the composite sync signal expected by NeoPico. Preferred sync source remains JAMMA/MVS edge pin P video sync, properly level shifted/conditioned.
- JAMMA/MVS sync pin location clarified: edge connector pin P is Video Sync on the solder side, between N=Video Green and R=Service, opposite component-side pin 13. Use this CSYNC source for NeoPico GP27 via level shifting/conditioning.
- MV1B/YM3016 digital audio answer: MV1B has YM2610 and YM3016. YM3016 digital inputs are SD pin 4, CLK pin 5, SMP2 pin 7, SMP1 pin 8. However this is Yamaha floating DAC data, not the current MV1C/BU9480F right-justified linear PCM path, so NeoPico would need a new YM3016 decoder mode. Also YM2610 SSG/PSG audio is analog-only from YM2610 ANA and is mixed after YM3016, so YM3016 digital alone gives FM+ADPCM, not full final Neo Geo audio. For full audio on YM3016-era boards, analog ADC after the mixer is simplest/complete, or combine YM3016 digital plus ADC SSG analog.

## 2026-07-06 - Trion T13 HDMI feasibility
- T13-class FPGA can definitely feed an external HDMI TX chip for 480p/720p: generate RGB/YCbCr + HS/VS/DE + PCLK + I2S, let ADV7513/ADV7511/IT661xx handle TMDS, audio packets, DDC/HPD. Direct HDMI/TMDS from T13 LVDS is possible-ish for 480p and marginal for 720p because T13 LVDS TX is 800 Mbps max while 720p60 TMDS is 742.5 Mbps/lane, plus serializer supports up to 8:1 not native 10:1 and LVDS is not TMDS-compliant. Prefer HDMI TX chip for product-quality output.
- Pico/RP2350 + HDMI TX chip feasibility: Pico can easily configure/control a TX over I2C and supply I2S. Driving the video side is the hard part: ADV7513-style full input is D[23:0]+CLK+HS+VS+DE, which with existing MVS 19-bit capture and 3 audio inputs exceeds/strains RP2350B pin budget. 480p or simple test patterns might be possible with a reduced-width/DDR/embedded-sync TX input, but 720p live NeoPico capture via external HDMI TX is not a clean Pico path. FPGA is the proper source for an HDMI TX chip.
- FPGA choice for NeoPico successor with HDMI TX: recommend T20 over T13. T13 likely enough for a minimal streaming design but pin/RAM/logic margin is tight, especially with 24-bit HDMI TX input. T20 gives more LEs/RAM and, importantly, LQFP144 with 97 GPIO for a hand-assembleable 24-bit HDMI-TX bus plus MVS inputs. T20F256/BGA gives plenty GPIO in smaller area if BGA is acceptable. T13 only makes sense for cost/size if using reduced-width DDR/YCbCr HDMI TX input and minimal features.
- User pushed back on T20 recommendation: T13 is likely more than enough when paired with RP2350/RP2040-class control and a real HDMI TX, given current single-RP2350 design already does 720p60. Adjust recommendation: T13 should be the default target if architecture stays streaming/no framebuffer and uses reduced-width or carefully budgeted HDMI TX bus. T20 is only for margin/future-proofing, not required.
- Image IC identification: board appears to use Lattice/Silicon Image SiI9022ACNU HDMI transmitter, Efinix Trion T20F256 FPGA, ISSI IS42S32400F-7BLI SDRAM, Espressif ESP32-C3-class MCU, Silicon Labs Si5351 clock generator, plus an unreadable small SPI flash/power IC. Red-boxed chip is HDMI TX, not FPGA.

## 2026-07-07 - Audio WS/frame-sync requirement
- User asked if WS is really necessary for audio, even mono. Answer: for I2S/right-justified/MVS digital audio, a frame-sync signal is required to know word/channel boundaries. Mono can duplicate one captured channel after decoding, but still needs WS or an equivalent strobe. Without WS, BCK+DATA can bit-slip or channel-slip and is not release-quality. For YM3016, SMP1/SMP2 are the equivalent frame/sample strobes, not optional if decoding digital DAC data.
- Online docs for WS/frame sync: NXP UM11732 I2S spec says 3-line bus uses SCK, WS, SD; WS indicates left/right and changes before MSB so receiver can store/clear words. Microchip I2S IP guide says WS/LRCK selects left/right and I2S can support mono but still has WS in receiver ports. NeoGeoDev YM3016 page lists SD, CLK, and SMP1/SMP2 alternating clocks for L/R sample-and-hold circuits from YM2610, i.e. YM3016's frame/sample strobes.

## 2026-07-07 - YM3016-era digital audio tap candidates
- Best digital audio candidates for MV1B/YM3016 boards: tap at YM3016 input side, not downstream analog. Primary pins: YM3016 pin 4 SD/DATA, pin 5 CLK/BCK, pin 8 SMP1, pin 7 SMP2. Bring both SMP1/SMP2 if possible because they are the L/R sample strobes and firmware can choose/derive frame sync. First minimal experiment can try SD+CLK+SMP1 as WS, then SMP2/invert if channel/framing is wrong. Reminder: YM3016 digital excludes YM2610 SSG/PSG analog, so full final audio still needs analog mix ADC or mixed approach.

## 2026-07-07 - PCM1802 firmware location/status
- User asked where the PCM1802 firmware is. Current local build artifacts only include build/src/neopico_hd.uf2 and build-snes-capture/src/neopico_hd.uf2, both with NEOPICO_AUDIO_PCM1802=OFF. PCM1802 support exists behind CMake option NEOPICO_AUDIO_PCM1802=ON, but no dedicated PCM1802 UF2 is currently present in the repo/build dirs. Need build a separate directory with -DNEOPICO_AUDIO_PCM1802=ON if user wants it.
- PCM1802 hardware mode recommendation: use PCM1802 as master so it generates BCK/LRCK for Pico capture. For 12.288MHz SCKI, set MODE1=1, MODE0=1 (master 256fs = 48kHz), FMT1=0, FMT0=1 (24-bit I2S), PDWN=1, BYPAS=0 normal HPF, OSR=0 safe default. Connect DOUT pin12->GP22, LRCK pin10->GP23, BCK pin11->GP24; FSYNC pin9 unused by current firmware. ECS-100X/100AC full-size oscillator: pin1 NC, pin7 GND/case, pin8 output to PCM1802 SCKI pin15, pin14 +5V, with 0.1uF close from 14 to 7 and short output wiring/optional 33-100R series.

## 2026-07-08 - PCM1802/ECS cap placement reminder
- User asked where caps go. Oscillator: 0.1uF directly between ECS-100AC pin14 +5V and pin7 GND, physically at the oscillator legs, optional 10uF across same rails if wires are long. PCM1802: VCC pin5 to AGND pin6 needs 0.1uF + 10uF close; VDD pin14 to DGND pin13 needs 0.1uF + 10uF close; VREF1 pin3 to AGND pin6 needs 0.1uF + 10uF close; analog inputs need 1uF AC coupling in series into VINL/VINR if taking analog audio.

## 2026-07-10 - MVS C2 reminder
- User asked what C2 is for on MVS. In cps2_digiav/NeoPico install wording, C2 is the 6MHz MVS video pixel clock/PCLK input, not a capacitor. C1 is the 12MHz clock. For NeoPico the equivalent is PCLK on GP28; MV1C tap PC23 pin 11, MV1B/GRC2 tap 6MB pin 2.
- C1 to Pico XIN guidance: technically yes, RP2350 XIN can be driven by a single-ended CMOS clock up to 50MHz and XOUT left disconnected. For NeoPico, C1=12MHz is the right frequency for existing firmware. Do not feed raw 5V MVS C1 directly into XIN; buffer/level-shift to clean 3.3V CMOS, add small series resistor, remove/disable onboard crystal and XIN/XOUT load caps if using XIN, leave XOUT floating. Still keep C2/6MHz PCLK to GP28 for capture. Risks: Pico cannot boot/use USB if MVS clock absent, power-domain backfeed if MVS clocks Pico while unpowered, and clock loss/reset can stall PLL/HSTX.

## 2026-07-10 - MVS build check
- User asked to build MVS firmware if not already. build/CMakeCache already had NEOPICO_CAPTURE_TARGET=MVS and NEOPICO_AUDIO_PCM1802=OFF. Reconfigured with -DNEOPICO_CAPTURE_TARGET=MVS and built target neopico_hd successfully. Artifact: build/src/neopico_hd.uf2.
- Copied MVS build artifact to ~/Desktop/neopico_hd_mvs.uf2 for user.
- Flash attempt for ~/Desktop/neopico_hd_mvs.uf2 failed: pi flash could not find an accessible RP-series device in BOOTSEL mode after reboot/load retries. Need user to connect/check USB/BOOTSEL before retrying.

## 2026-07-10 - SNES target CI/release prep
- User requested CI check, commit/push, and release for the staged NeoPico SNES capture target work.
- Fixed `.github/workflows/build.yml` so CI builds both `NEOPICO_CAPTURE_TARGET=mvs` and `snes`, uploads target-specific UF2/ELF artifacts, and release downloads both build artifacts by pattern.
- Local validation: YAML parsed, artifact copy paths checked, pre-commit passed, local CI-equivalent MVS and SNES Release builds passed in `build-ci-mvs` and `build-ci-snes`. KiBot CLI not installed locally, so fabrication job was not runnable outside GitHub Actions.
- Commit/release scope should include firmware/source + workflow only. Keep SCRATCHBOOK, hardware/KiCad dirt, build dirs, and history dirs out of commit.

## 2026-07-10 - Release v0.8.2 published
- Committed staged firmware/workflow changes on main as 99b3c2b (`capture: add selectable MVS and SNES targets`) and pushed origin/main.
- Created and pushed annotated tag `v0.8.2` from 99b3c2b. Changelog since v0.8.1 includes audio flashcart recovery, quantized DARK/SHADOW LUT, and selectable MVS/SNES capture targets.
- GitHub Actions tag run 29114989756 passed: MVS build, SNES build, KiBot fabrication, and release publication all succeeded.
- Published release assets include `neopico_hd_mvs.uf2`, `neopico_hd_snes.uf2`, corresponding ELF files, and `neopico-hd-jlcpcb.zip`.

## 2026-07-11 - AES3-6 CJMCU PCM1802 wiring verification
- User's AliExpress item 1005009291049868 is the CJMCU-style PCM1802 board with header order `SCK, PDW, LRCK, FSY, BCK, DOUT, GND, 3.3V, +5V` and separate `LIN`/`RIN` analog inputs.
- For NeoPico PCM1802 firmware and a 12.288MHz ECS-100-series oscillator: configure master 256fs and 24-bit I2S with MODE1=1, MODE0=1, FMT1=0, FMT0=1, BYPAS=0, OSR=0. Open control pads are low due PCM1802 pulldowns; bridge the marked `+` side for MODE1, MODE0, and FMT0 only, after checking that the module's `+` jumper rail has continuity to 3.3V because some clones leave it disconnected.
- Digital wiring: DOUT->GP22, LRCK->GP23, BCK->GP24, FSY unused, PDW->3.3V. Supply module +5V from regulated AES 5V, 3.3V from NeoPico 3V3, and use a common GND. ECS oscillator pin14->regulated +5V, pin7->GND, pin8->SCK, pin1 NC, with 0.1uF from pin14 to pin7. PCM1802 SCKI is specified 5V tolerant.
- AES3-6 analog taps are the Audio L and Audio R points marked in cps2_digiav `board/neogeo/doc/aes3-6_hookup_points.jpg`; connect them to module LIN and RIN respectively. The breakout already contains input coupling/filter components, so no additional series input capacitors are normally needed.
- ECS-100 package clarification: it has four physical legs using DIP-14 position numbers 1, 7, 8, and 14. Do not imply that it has 14 physical legs.
- ECS-100 orientation correction: user correctly identified the top view as `14  8 / 1-dot  7`. The ECS datasheet package drawing is explicitly a bottom view (`1  7 / 14  8`). Previous assistant top-view diagram was wrong. Use pin functions, dot at pin 1, and remember the datasheet view distinction.

## 2026-07-12 - AES PCM1802 harsh audio: root cause found in SRC rate servo
- User installed NeoPico in AES 3-6 with PCM1802 (master 256fs, 12.288MHz ECS osc = exactly 48000 Hz I2S). Running v0.7.2 PCM1802 build; sound works but is harsher than real analog/emulator.
- Root cause (code-provable, affects v0.7.2 AND main identically): `NEOPICO_AUDIO_PCM1802` only switches the PIO program and bit unpacking. The SRC still inits at 55556 (v0.7.2: hardcoded in src.h; main: CAPTURE_AUDIO_INPUT_RATE via capture_profile.h) and the DI-queue feedback servo in audio_subsystem.c clamps input_rate to 53000-58000 (MVS window). With a true 48kHz source the servo pins at the 53000 floor forever.
- Effect: DROP-mode SRC (MVS default) emits 48000/53000 of samples = discards ~9.4% (every ~11th sample -> discontinuity fizz with ~4.3kHz-spaced sidebands), and output runs at ~43.5k samples/s vs 48k consumed, so hstx_di_queue underruns and splices pre-encoded 4-sample silence packets (~1100/s) = chopping. Tempo/pitch stay correct overall, hence "works but harsh".
- `AUDIO_INPUT_RATE` (48000 under the flag, audio_config.h) is a dead define: nothing consumes it. SRC-mode cycle buttons (audio_pipeline_poll_buttons) are dead code on main - no runtime A/B possible.
- Verified: main + `-DNEOPICO_AUDIO_PCM1802=ON` configures and builds clean (scratchpad build-pcm1802). Releases only ship mvs/snes UF2s, which is why v0.7.2 is the user's only I2S-capable UF2 (it was a local build).
- Proposed fix (presented, awaiting user go; all gated on NEOPICO_AUDIO_PCM1802, MVS/SNES untouched): (1) SRC_INPUT_RATE_DEFAULT -> AUDIO_INPUT_RATE; (2) servo clamp -> 47000/49000 under the flag; (3) default SRC mode LINEAR under the flag - DROP can never emit more samples than input so it cannot compensate when the ADC crystal runs ppm-slow vs Pico, LINEAR can.
- Secondary harshness candidates if any remains after fix: AES tap point is pre-output-filter (brighter than RCA out; could add gentle LPF later), ADC clip on hot tap level. Judge after servo fix.

## 2026-07-12 - PCM1802 SRC servo fix implementation
- User explicitly approved implementation of the flag-gated PCM1802 SRC fix.
- Changed only PCM1802 behavior: initialize the SRC from `AUDIO_INPUT_RATE` (48 kHz), constrain its feedback servo to 47-49 kHz, and select `SRC_MODE_LINEAR` by default. MVS and SNES rate defaults and bounds remain unchanged.
- Validation passed: rebuilt existing MVS and SNES targets, then configured and built a fresh Release MVS + `NEOPICO_AUDIO_PCM1802=ON` target. PCM1802 artifact: `/tmp/neopico-hd-pcm1802/src/neopico_hd.uf2`.
- No hardware commands or flashing performed.
- User acknowledged the exact flash command. `pi flash /tmp/neopico-hd-pcm1802/src/neopico_hd.uf2` completed successfully on RP2350, and the board rebooted into application mode.
- Hardware result confirmed by user: PCM1802 audio now sounds great after the 48 kHz SRC initialization, 47-49 kHz servo bounds, and LINEAR-mode fix.
- Committed the four PCM1802 audio source changes on `main` as `52d9375` (`audio: fix PCM1802 sample-rate conversion`). Pre-commit hooks passed. Excluded SCRATCHBOOK, hardware/KiCad changes, histories, libraries, and build artifacts.

## 2026-07-12 - AES controller input plan for OSD
- User proposes UP, DOWN, START/confirm, and SELECT/back. Recommended behavior: hold START+SELECT to open OSD, then UP/DOWN navigate, START confirms, SELECT returns, avoiding OSD activation on ordinary START presses.
- Current firmware only reads the two physical OSD inputs GP25 MENU and GP26 BACK; four-input navigation requires firmware changes, kept entirely in the Core 1 background OSD task.
- Existing PCB U5 (`SN74LVC245APW`, 3.3 V, A-to-B) has six unused 5 V-tolerant translator channels. Proposed safe taps: AES DB15 UP pin15 -> U5 A3/pin4 -> B3/pin16 -> GP20; DOWN pin7 -> A4/pin5 -> B4/pin15 -> GP21; START pin11 -> A5/pin6 -> B5/pin14 -> GP9; SELECT pin3 -> A6/pin7 -> B6/pin13 -> GP10. Use common GND; do not connect controller +5 V to Pico. GP9/10/20/21 are currently unconnected, preserving GP25/26 physical buttons.
- User noted GP0-GP7 are already broken out on J5. PCB/source verification confirms J5 pins 1-8 map directly to GP0-GP7 and firmware currently does not use them. Revised preferred controller mapping: translated UP/DOWN/START/SELECT to J5 pins 1/2/3/4 (GP0/1/2/3), leaving J5 pins 5-8 free and GP25/26 physical OSD buttons unchanged.
- Correction after user challenge: RP2350 GPIO0-GPIO7 are `Digital IO (FT)`, unlike RP2040 GPIO. The RP2350 datasheet permits up to 5.5 V on FT inputs when IOVDD is powered at 3.3 V, so AES controller lines may connect directly to J5 GP0-GP3 during normal jointly-powered operation. Previous statement that level shifting was always required was wrong. Important limit: with IOVDD unpowered, FT input maximum is 3.63 V; use translation/isolation if AES may drive 5 V while NeoPico is off. Configure as input, no internal pull, active-low; optional 1-4.7k series resistors provide fault/ESD current limiting.
- AES3-6 controller tap recommendation: use Player 1 connector `CN1` solder tails or nearby pads, verified by continuity to the front contacts while fully unpowered. Front view looking into console port is top row `8 7 6 5 4 3 2 1`, bottom row `15 14 13 12 11 10 9`. Relevant pins: UP 15, DOWN 7, START 11, SELECT 3, GND 1. Preferred direct mapping is CN1 pins 15/7/11/3 to J5 pins 1/2/3/4 (GP0/1/2/3); do not connect controller +5 V pin 8.
- Controller tap refinement: CN1 solder tails are on the cable side, before the AES series EMI/filter components. They work electrically for slow active-low inputs, but the preferred tap is the console/NEO-B1 side of each corresponding filter so the RP2350 benefits from the AES cable EMI/ESD filtering. The GP input is high impedance, so either side does not materially load game controls; after-filter is safer/cleaner.
- More accessible AES controller taps identified at the through-hole CRE401 SIP networks after the filters. Player 1 mapping from the AES controller schematic: `CR5 pin 3 / OUT1 / IN00` = UP, `CR5 pin 5 / OUT2 / IN01` = DOWN, `CR3 pin 3 / OUT1 / IN20` = START, `CR3 pin 5 / OUT2 / IN21` = SELECT. Preferred wiring: those four pads to J5 GP0/GP1/GP2/GP3. CR3 and CR5 are the red 10-pin packs above CN1; use their larger underside solder pads, identify square pad 1 and count pin numbers, and verify against CN1 with the console fully unpowered because underside orientation is mirrored.
- Terminology correction: AES controller inputs are handled by `NEO-C1`, not `NEO-B1`. Prior "console/NEO-B1 side" wording means the downstream console-logic side of the filter, specifically toward NEO-C1.

## 2026-07-12 - AES START/SELECT OSD input implementation
- User wired START and SELECT to GP0/GP1 but is not fully certain of their order. Initial firmware assumption is START=GP0 and SELECT=GP1; pin numbers are CMake cache settings so they can be swapped without source edits.
- Added `NEOPICO_OSD_CONTROLLER_INPUTS` default OFF. When enabled, GP0 is an additional MENU input and GP1 an additional BACK input, while physical GP25/GP26 buttons remain active. Controller GPIOs are inputs with internal pulls disabled and are sampled only in the existing Core 1 OSD background task.
- Validation passed: pre-commit hooks, default MVS build, SNES build, and fresh Release PCM1802 + controller-input build. Candidate UF2: `/tmp/neopico-hd-pcm1802-controller/src/neopico_hd.uf2`, configured START/MENU=GP0 and SELECT/BACK=GP1.
- Pending coordinated flash. If controls are reversed in hardware, rebuild with MENU pin 1 and BACK pin 0.
- User acknowledged the exact command. Flashed `/tmp/neopico-hd-pcm1802-controller/src/neopico_hd.uf2` successfully with `pi flash`; RP2350 rebooted into application mode. Awaiting hardware confirmation of GP0/GP1 button order.
- Workflow correction from user: do not place NeoPico build directories/artifacts under `/tmp`; use `main` and a build directory inside the repository. Source edits were already on repo `main`, but the external temporary build path was contrary to preference. Use a repo-local build path for subsequent builds and flashes.

## 2026-07-13 - Controller chord, swap, and debounce
- Hardware result: original assumption was reversed. Correct mapping is START/MENU=GP1 and SELECT/BACK=GP0.
- Updated controller OSD behavior: while hidden, START alone does nothing and a debounced START+SELECT chord opens the root menu once. While visible, START remains MENU/confirm and SELECT remains BACK/cycle.
- Added configurable 30 ms per-input stable-state debounce and press-transition events, so holding a controller button cannot repeat/toggle actions. Controller inputs remain flag-gated OFF by default; physical GP25/GP26 buttons retain existing behavior.
- Validation passed: pre-commit hooks, default MVS build, SNES build, and PCM1802/controller Release build. Repo-local artifact: `build-pcm1802-controller/src/neopico_hd.uf2`, configured GP1 START, GP0 SELECT, 30 ms debounce. Pending coordinated flash.
- User acknowledged the repo-local flash command. `pi flash build-pcm1802-controller/src/neopico_hd.uf2` completed successfully and rebooted the RP2350 into application mode.
- Hardware result: 30 ms stable-low debounce misses some START/SELECT presses. Root cause is the implementation requiring the pressed level to survive two Core 1 background polls at least 30 ms apart; audio work runs before the OSD poll, so short taps can disappear between polls.
- Proposed correction, awaiting approval: generate the press event immediately on the first active-low edge, then lock it out until the input has remained released for about 20 ms. This preserves one event per physical press and bounce protection while eliminating the stable-low capture delay that loses taps.

## 2026-07-12 - PCM1802 servo fix: user implemented, reviewed and verified
- User implemented the proposed fix themselves (audio_config.h AUDIO_INPUT_RATE_MIN/MAX 47000-49000 under flag, src.h SRC_INPUT_RATE_DEFAULT=AUDIO_INPUT_RATE, subsystem clamp uses new macros, LINEAR default for PCM1802||SNES). Review: correct, matches proposal exactly.
- Verified with patch applied: (1) PCM1802=ON build compiles clean; (2) flag-OFF MVS Release build is byte-identical to pre-patch build-ci-mvs UF2 except one byte, the embedded __DATE__ string (Jul 12 vs Jul 10). Zero code regression for MVS/SNES.
- Deliverable: ~/Desktop/neopico_hd_pcm1802.uf2 (main + patch + NEOPICO_AUDIO_PCM1802=ON, Release, default MVS capture target). Awaiting hardware listen test on the AES.
- Reminder: previous pi flash attempt (2026-07-10) failed with no BOOTSEL device; Pico is installed in the AES, user must connect USB first.

## 2026-07-12 - Occasional audio drops (all fw, MVS+AES): code assessment
- User reports rare audio drops on both consoles, both before and after the PCM1802 servo fix. Investigation only, no code changed.
- Leading mechanism (present in ALL versions, both consoles, sink-dependent): DI scheduler paces audio by samples_per_line_fp = floor(48000*h_total*2^16/pixel_clock) (video_output_rt.c configure_audio_packets; same in video_output.c). The 16.16 floor makes delivered rate ~47999.8 samples/s, while ACR N/CTS (6144, pclk/1000) advertises exactly 48000.000 on the SAME clock. Deterministic deficit ~0.18 samples/s at 480p/240p (~0.42 at 720p, floor is always low so the sink always DRAINS). Sink buffer underflows every ~10min-hours depending on TV -> brief concealment mute = "drop every once in a while". Firmware buffers stay healthy (servo locks producer to scheduler), so no firmware counter sees it. Fix direction if confirmed: exact rational Bresenham pacing (480p is exactly 32/21 samples/line) or 32.32 accumulator; do NOT try to fix via CTS (1 CTS step = ~1.9Hz, too coarse).
- Mechanism 2 (current main only, NEOPICO_EXP_AUDIO_REARM_ON_VIDEO_REACQUIRE=ON): one vsync gap >100ms (MVS_NO_SIGNAL_TIMEOUT_MS) -> capture reset -> on reacquire audio rearm: mute + ~0.5s REWARM. Distinct signature: half-second mute coinciding with a video hiccup. Cannot explain v0.7.2-era drops (path didn't exist).
- Mechanism 3 (all versions): Core-1 background stall > queue cushion (target level 128 pkts = ~10.7ms) -> ISR splices 4-sample silence packets; hstx_di_queue_silence_count counts exactly this but is only displayed on the genlock diag screen (compiled out, NEOPICO_EXP_GENLOCK_DYNAMIC=OFF). Known stall sources are OSD-open snprintf redraws and flash settings writes (user-triggered), so unlikely during plain gameplay.
- Discriminators proposed to user: (a) drop character/duration + video coincidence -> mech 2; (b) rough periodicity in attract mode + varies by TV -> mech 1; (c) tiny flag-gated diag surfacing silence_count on selftest OSD would prove/disprove mech 3 and 1 (counter climbs during audible drop = producer side; silent counter = sink side).

## 2026-07-12 - Audio-drop tooling implemented via delegated agent, reviewed
- User committed the PCM1802 servo fix as 52d9375, then asked to delegate the diag+pacing work to a Sonnet subagent and review.
- Agent delivered two flag-gated features, both OFF by default:
  - NEOPICO_DIAG_AUDIO_OSD (src/CMakeLists.txt + menu_diag_experiment.c): renders "AU<count>" (hstx_di_queue_silence_count) at row 14 col 2 of the selftest OSD screen, inside the existing screen-gated 1 Hz update block. Row 14 verified free in all flag combos.
  - PICO_HDMI_EXACT_AUDIO_PACING (pico_hdmi submodule, LEFT DIRTY/uncommitted): hstx_di_queue_set_samples_per_line_exact(spl, rem, den) + Bresenham remainder in tick() (+1 accum LSB when rem_accum wraps den). RT path computes rem/den from 64-bit num = rate*h_total<<16. Long-term delivery = exactly sample_rate; correction granularity 1/65536 sample.
- Review verdict: correct. Math verified (avg units/line = spl + rem/den = exact). ISR additions are add/compare/subtract only, state in .data like existing. Stale-state fallback rem=0/den=1 is a no-op. set_sample_rate -> exact-setter ordering in configure_audio_packets leaves consistent final state.
- Independent verification: default MVS build byte-identical to pre-edit baseline (cmp clean); featured build (PCM1802+DIAG+PACING) compiles clean, new symbols present in featured ELF and absent in default. Featured UF2 is +12.8KB (snprintf pull-in from the diag; build is copy_to_ram so flash-alignment sensitivity does not apply).
- Nit (accepted): diag buf[10] truncates counts >999999; harmless for a diagnostic.
- Deliverable: ~/Desktop/neopico_hd_pcm1802_diag_pacing.uf2. Interpretation: drops vanish = sink-drift confirmed (pacing is the fix); drops persist + AU climbs at drop time = producer starvation; drops persist + AU flat = look at rearm path or the TV.
- Pending when pacing proves out: commit pico_hdmi changes upstream (fliperama86/pico_hdmi) + submodule bump + optionally enable the pacing flag by default. Not done (no commits without explicit ask).

## 2026-07-12 - Exact audio pacing made default; pico_hdmi pushed
- User: soak looks fine so far, make exact pacing the default; also merge upstream pico_hdmi and push.
- Upstream check: origin/2.1-dev had zero new commits; origin/main had been fast-forwarded to f8034c7 and tagged v0.0.7 upstream, and the remote 2.1-dev branch had been deleted (local tracking ref was stale, which briefly caused a push to recreate 2.1-dev; deleted it again and pushed to main instead).
- pico_hdmi: PICO_HDMI_EXACT_AUDIO_PACING default flipped to ON, committed as b6422ee on main (fast-forward from f8034c7), pushed to fliperama86/pico_hdmi. Rollback documented: -DPICO_HDMI_EXACT_AUDIO_PACING=OFF.
- neopico-hd: commit 267ae19 on main (submodule bump to b6422ee + NEOPICO_DIAG_AUDIO_OSD default OFF). NOT pushed, user still soaking; push/release when ready (submodule side is already pushed so CI will resolve).
- Verified before committing: fresh no-flags MVS build contains the pacing symbol (default really ON); build with explicit OFF is byte-identical to the pre-change baseline (clean rollback); pre-commit hooks passed.
- User's currently flashed soak UF2 (pcm1802+diag+pacing) is functionally the new default plus diag, so the soak remains representative.

## 2026-07-12 - Why exact pacing was off by default (user question)
- Answer: the flag never pre-existed; it was created today OFF-by-default per AGENTS.md experiment policy (flag-gated, byte-identical default, flip after hardware soak), then promoted.
- Pacing lineage in pico_hdmi: 6700cec set_sample_rate assumed exactly 60 Hz (exact for 480p, 25.2MHz/800x525); d984db4 240p landed with known audio issues; e2a4022 (2026-02-07) fixed the 60.114 Hz mismatch (~91 excess samples/s, periodic dropouts) with floor-truncated 16.16 spl, leaving the ~0.2 samples/s residual; b6422ee (today) removes the residual with the rational remainder.

## 2026-07-12 - Exact pacing coverage across modes (user question)
- Verified per-mode: configure_audio_packets runs at init, on apply_mode (runtime switch), and on sample-rate change, so each mode derives rem/den from its own h_total/pixel clock. Old deficits: 480p 0.183/s, 240p 0.183/s, 720p 0.089/s; now exactly 48000.000 in all three. CTS=pclk/1000 is integer-exact for 25.2MHz and 74.4MHz, so delivered == ACR-advertised in every shipped mode.
- Scope limits: non-RT builds (video_output.c) still floor-truncate (not used by selector firmware); genlock experiments slew CTS by design (flags OFF); the fix addresses only the pacing-drift drop mechanism, not vsync-loss rearm mutes or Core-1 starvation (AU diag covers the latter).

## 2026-07-12 - Documentation pass for the audio work (commit cc6cd94)
- HSTX_IMPLEMENTATION.md: new "Pacing & Clock Accuracy" + "Underrun Diagnostics" subsections under HDMI Audio (pacing derivation, truncation-drift history e2a4022 -> b6422ee, PICO_HDMI_EXACT_AUDIO_PACING default ON + rollback, AU counter usage).
- KNOWN_ISSUES.md: added "5. Rare Periodic HDMI Audio Dropouts (Resolved 2026-07-12)" with root cause, fix commits (pico_hdmi b6422ee, neopico 267ae19), and the two remaining distinct mechanisms (rearm mute, Core-1 starvation + AU diag).
- MVS_MV1C_DIGITAL_AUDIO.md: new "Analog Sources (PCM1802 / AES builds)" subsection (source-aware servo window 47000-49000, pre-52d9375 harshness bug, why LINEAR is the PCM1802/SNES default).
- Verified no em dashes in added lines; pre-commit hooks passed. Not pushed (main still unpushed by design; user pushes when soak is done). User separately committed hardware work as b7ccd12.

## 2026-07-13 - Runtime Audio menu implementation in progress
- User approved a flag-gated persistent Audio menu with `MV1C Digital` and `PCM1802 I2S`, applied by reboot, plus the missed-controller-press correction.
- Design keeps the settings payload/version at 32 bytes: audio source occupies two formerly reserved bytes and uses marker `0xA5`, so old records retain their resolution and fall back to the build's compile-time audio default.
- Added `NEOPICO_AUDIO_RUNTIME_SELECT` default OFF. It is limited to the MVS runtime-mode OSD/settings build; default MVS and SNES builds remain on their compile-time audio paths.
- Controller handling now fires on the first observed active-low edge, suppresses repeats while held/bouncing, and rearms only after a stable release. Default release debounce is 20 ms.
- Tooling correction: plain `clang-format` was not on PATH; use the repository hook's `/opt/homebrew/opt/llvm/bin/clang-format` fallback directly.
- Review caught and fixed a flag interaction before hardware testing: a runtime `MV1C Digital` choice must explicitly use DROP mode even when `NEOPICO_AUDIO_PCM1802=ON` makes PCM1802 the old-settings fallback.
- Persistence review caught an old-record edge case: confirming the active compile-time fallback now writes the audio-source marker and current resolution, instead of returning without actually recording the explicit choice.
- Validation passed: featured Release build (`PCM1802=ON`, runtime Audio selector ON, controller GP1/GP0, 20 ms release debounce), flag-off MVS Release, flag-off SNES Release, `git diff --check`, clang-format, and clang-tidy/pre-commit.
- Repo-local candidate: `build-pcm1802-controller/src/neopico_hd.uf2`, SHA-256 `b1fe9f98c4ccad22d13ff694a390b50cfa5c7574b465b7bb6346b706771ec24f`. No hardware command or flash performed.
- User acknowledged the exact repo-local flash command. `pi flash build-pcm1802-controller/src/neopico_hd.uf2` completed successfully and rebooted the RP2350 into application mode. Awaiting controller debounce and persistent Audio menu hardware verification.
- Hardware verification: controller behavior and Audio menu work, but the first-run runtime Audio fallback incorrectly appeared as `PCM1802 I2S`. User correction: runtime-select firmware must default to `MV1C Digital`; only an explicitly persisted selection may override it. `NEOPICO_AUDIO_PCM1802` should remain the source choice only for builds without the runtime selector.

## 2026-07-13 - Compile-flag cleanup audit
- User requested cleanup because the CMake surface has accumulated many stale flags. Audit found roughly 25 April-era timing-isolation/A-B flags still threaded through CMake/source despite all being default OFF and absent from CI/release builds.
- Recommended cleanup: delete obsolete selector isolation, background-disable, audio pipeline bypass, legacy InfoFrame/alignment, manual rearm, capture-freeze, soak-open, stress, and deprecated alias flags; normalize source to the current production behavior.
- Recommend replacing the interacting `NEOPICO_AUDIO_PCM1802` + `NEOPICO_AUDIO_RUNTIME_SELECT` pair with one audio-mode cache string (`DIGITAL`, `PCM1802`, or `SELECTABLE`). MVS `SELECTABLE` defaults to MV1C unless flash contains an explicit source; SNES remains digital.
- Keep genuinely active build/product choices and ongoing experiments: capture target, output modes/non-RT, copy-to-RAM, OSD/selftest/settings/controller, DARK/SHADOW, test/diagnostic outputs, precomposed HDMI, genlock, triple-ASM, and audio underrun OSD.

## 2026-07-13 - Compile-flag cleanup implemented and validated
- User approved the audited cleanup. Removed about 25 obsolete A/B, timing-isolation, audio-bypass, selector-soak, manual-rearm, alignment, and capture-freeze CMake flags plus their dead source branches. Current production audio startup/rearm/frame-resync behavior is now direct code.
- Replaced `NEOPICO_AUDIO_PCM1802` plus `NEOPICO_AUDIO_RUNTIME_SELECT` with `NEOPICO_AUDIO_MODE=DIGITAL|PCM1802|SELECTABLE`. Fresh MVS defaults to `SELECTABLE`; fresh SNES defaults to `DIGITAL`; fixed AES builds use `PCM1802`.
- Corrected selectable first-run behavior: the in-memory source initializes to `AUDIO_SOURCE_MV1C_DIGITAL`; flash overrides it only when the explicit audio-source marker is valid. A previously persisted PCM selection remains honored and must be changed once through the menu if MV1C is desired.
- Renamed stable product flags: `NEOPICO_RESOLUTION_MENU`, `NEOPICO_RESOLUTION_MENU_720P`, and `NEOPICO_FIRST_BOOT_REBOOT`. Remaining `NEOPICO_EXP_*` flags are only active precomposed/genlock/VTOTAL experiments.
- Updated README build recipes, audio/resolution/glitch docs, and firmware audit reporting for the simplified flag surface.
- Validation passed: repo-local featured selectable/controller build, fresh default MVS (`SELECTABLE`), fresh default SNES (`DIGITAL`), fixed MVS PCM1802, `git diff --check`, formatting hooks, and firmware timing/layout audit with no findings. Clang-tidy remains blocked by its host-Clang versus ARM-sysroot header mismatch; its one unsafe auto-fix was reverted and the target rebuilt successfully.
- Current repo-local candidate: `build-pcm1802-controller/src/neopico_hd.uf2`, SHA-256 `7aa4fa459ce7aaa74ead25a525ba0010bc13a4508c3bf89a8553aac6eb8b9b11`. Not flashed during this cleanup.

## 2026-07-13 - v0.9.0 release scope
- User requested pushing `main` and publishing a release. Selected `v0.9.0` because the firmware adds controller-driven persistent Audio selection and replaces public CMake audio/resolution flags.
- Release scope is the tested firmware, documentation, audit, and memory changes. Explicitly excluded unrelated local KiCad UI-state change `hardware/neopico-hd/neopico-hd.kicad_prl`.
- Repository workflow builds fresh MVS and SNES UF2/ELF artifacts plus fabrication files from a `v*` tag, then creates the GitHub release automatically.
- Release result: committed tested scope as `937d887`, pushed `main`, and passed branch Actions run `29266678886` for MVS, SNES, and fabrication.
- Created and pushed annotated tag `v0.9.0`; tag Actions run `29266842188` passed all build/fabrication/release jobs. GitHub published the non-draft release with MVS/SNES UF2+ELF artifacts and `neopico-hd-jlcpcb.zip`: https://github.com/fliperama86/neopico-hd/releases/tag/v0.9.0
- GitHub Actions emitted only Node.js 20 deprecation warnings for `actions/checkout@v4` and `actions/upload-artifact@v4`; no job failed. The unrelated local KiCad `.kicad_prl` change remains uncommitted.

## 2026-07-13 - Firmware version in root OSD title
- User requested the version number beside the `NeoPico-HD` root-menu title.
- Implementation uses the root CMake `project(... VERSION 0.9.0 ...)` as the single version definition, passes it as `NEOPICO_VERSION`, and renders `NeoPico-HD v0.9.0` within the existing 28-column title row.
- Scope is root menu only; leaf-screen titles and the self-test title remain unchanged.
- Validation passed: selectable/controller MVS, fresh-default MVS, fresh-default SNES, exact ELF title-string checks, formatting/diff checks, and firmware timing/layout audit with no findings. Repo-local UF2 SHA-256: `6904d6a3ea7bcf6551d30698a8dd8cdefed499c06bb1dc40458a2391db8fef62`.
- No flash, commit, push, or release performed for this version-label change. The unrelated KiCad `.kicad_prl` modification remains untouched.

## 2026-07-13 - Four-button AES OSD navigation
- User corrected the final AES controller mapping to START=GP0 and SELECT=GP1; UP/DOWN remain GP2/GP3.
- Approved behavior: START+SELECT opens the hidden OSD, UP/DOWN moves and wraps root/Resolution/Audio selections, START confirms, and SELECT returns or cancels. GP25/GP26 retain legacy two-button confirm/cycle behavior.
- Implementation remains flag-gated by `NEOPICO_OSD_CONTROLLER_INPUTS` and runs only in the existing Core 1 background OSD task.
- Flash requested after repository-local build validation; hardware command requires a separate ready acknowledgement.
- Validation passed: targeted selectable/controller MVS build, firmware timing/layout audit, explicit compiled pin-definition check, `git diff --check`, and controller-off MVS/SNES regression builds.
- Repo-local candidate: `build-pcm1802-controller/src/neopico_hd.uf2`, SHA-256 `7830b84e14938734013d638b43b19aea71f68757f1580b139427fc59b843823f`. Pending operator acknowledgement for `pi flash`.
- Hardware result after user-performed flash: START works, but UP/DOWN are electrically reversed and SELECT does not respond. Correct installed direction mapping is UP=GP3 and DOWN=GP2.
- SELECT is wired from GP1 to MV1C NEO-YSA2 pin 54 (`SEL UP`). This is the MVS Select Game Up input corresponding to JAMMA parts-side pin 26 and is a valid P1 Select source only when the controller DB15 pin 3 drives that net. Recommended discriminator: measure GP1 released versus Select held before changing input logic.
- Rebuilt after the direction correction with compiled mapping START=GP0, SELECT=GP1, UP=GP3, DOWN=GP2. Firmware audit and `git diff --check` passed. Candidate SHA-256: `67edb7c923373a34aeaa44c01e601ea69ff002f22d915ec6aa6c9558bab6c13b`.

## 2026-07-13 - 240p selector regression investigation
- User reports 720p works but 240p does not, and requires all selector resolutions to use the same application path.
- Read-only inspection found the PicoHDMI RT engine and reboot selection are shared, but scanline routing was split: 240p/480p used a duplicated specialized callback while 720p used the unified three-mode callback.
- User approved consolidating 240p/480p/720p onto the unified callback, keeping it in scratch RAM, removing the duplicate, and validating builds/audits before any flash.
- Validation caught an initial no-720 selector link failure: the old preprocessor guard omitted both callbacks after the duplicate was removed. Corrected the unified callback guard to compile for every non-precomposed configuration; keep this edge build in regression coverage.
- Final implementation routes 240p, 480p, and 720p selector boots through one `video_pipeline_scanline_callback_reboot_modes` callback and removes the duplicated 240p/480p callback. The unified callback is at scratch-X `0x20080020`, size `0x1e0`; total scratch-X is `0x674`, leaving 396 bytes before the Core 1 stack boundary.
- Updated `audit_firmware.py` to select mode-specific critical symbols from CMake flags instead of expecting removed or 720p-only symbols in every build.
- Validation passed: selectable/controller MVS target, standard MVS, standard SNES, and selector-without-720 edge build; all applicable firmware audits report no findings. Formatting, `git diff --check`, Python compile check, and all pre-commit hooks passed.
- Repo-local candidate: `build-pcm1802-controller/src/neopico_hd.uf2`, SHA-256 `1c9989232c41b8d51750e16f0beb3acfc22831a765a28388488f7cda0972dfa8`. Not flashed.
- User acknowledged the exact flash step. `pi flash build-pcm1802-controller/src/neopico_hd.uf2` loaded successfully and rebooted the RP2350 into application mode. Awaiting 240p/480p/720p hardware verification.
- Hardware result: 240p produces a black picture while HDMI audio continues, and the persisted bad mode prevents normal resolution recovery. User requested a recovery chord and defined factory defaults as 480p plus MV1C Digital audio.
- Approved implementation: hold the physical MENU (GP25) and BACK (GP26) inputs together for at least 5 seconds, independent of OSD state; persist explicit 480p/MV1C defaults and reboot into 480p. This is a recovery mechanism, not yet a fix for the black 240p active-video path.
- Validation note: pre-commit clang-tidy again hit the known host-Clang versus ARM-sysroot missing-header failure and applied an unsafe internal-linkage fix to the externally called resolution-confirm function. Reverted that one auto-fix before rebuilding.
- Factory-reset implementation complete: `settings_factory_reset()` persists resolution byte 0, audio source byte 0 (`MV1C Digital`), and the explicit `0xA5` audio marker; the 5-second physical chord calls it before a watchdog reboot request for 480p.
- Validation passed: selectable/controller MVS, standard MVS, standard SNES, and no-720 selector builds; all firmware audits report no findings. Formatting, whitespace/EOF hooks, and `git diff --check` pass. Full pre-commit is blocked only by the known host clang-tidy ARM-sysroot failure; its unsafe fix was reverted and the target rebuilt successfully.
- Repo-local candidate: `build-pcm1802-controller/src/neopico_hd.uf2`, SHA-256 `e810da862e8c1de1638b937779b1e7d251d57b585e2b4bcb1406d39766736f68`. Not flashed.
- User acknowledged the exact flash command. `pi flash build-pcm1802-controller/src/neopico_hd.uf2` completed successfully and rebooted the RP2350 into application mode. Awaiting physical 5-second MENU+BACK recovery test from the persisted 240p state.
- Read-only comparison found the black-video regression: the v0.9 compile-flag cleanup removed the proven `PICO_HDMI_LEGACY_240P_AVI_INFOFRAME=1` propagation. The flashed build therefore used VIC-0 `PB1=0x10`, `PB2=0x18`, `PR=3`; prior full-stream hardware A/B showed that profile fails while conservative `PB1=0x00`, `PB2=0x08`, `PR=0` works with HDMI audio. Exact failing sub-field was not isolated.
- User approved restoring the conservative AVI profile for 240p only, while retaining the unified 240p/480p/720p application scanline callback and leaving standard 480p/720p metadata unchanged. No hardware action is authorized by this approval; rebuild and audit before requesting a separate flash acknowledgement.
- Implemented the production fix by always compiling PicoHDMI with `PICO_HDMI_LEGACY_240P_AVI_INFOFRAME=1`. Linked disassembly confirms VIC 0 takes the zeroed PB1 / PB2=8 branch and `build_all_command_lists()` passes PR=0; nonzero VICs retain PB1=0x10 and their existing 4:3/16:9 PB2 values.
- Validation passed for the selectable/controller MVS candidate, standard MVS, standard SNES, and selector-without-720 edge build. Every generated PicoHDMI flags file contains the compatibility define and every firmware timing/layout audit reports no findings. The targeted hot layout is unchanged at `.scratch_x=1652` with the unified callback at `0x20080020`, size 480 bytes.
- Candidate `build-pcm1802-controller/src/neopico_hd.uf2` SHA-256 is `c348fba1c3563cc8c03e919fef2470ac2766ee493264616f5e77e02b9ad2ca5e`. Not flashed; request an explicit operator acknowledgement before `pi flash`.
- User acknowledged the exact flash command. `pi flash build-pcm1802-controller/src/neopico_hd.uf2` loaded the compatibility candidate to 100% and rebooted the RP2350 into application mode. Awaiting hardware verification of 240p video with audio and subsequent 480p/720p switching.

## 2026-07-13 - v0.9.1 release preparation
- User reported the flashed 240p compatibility candidate is perfect and explicitly requested pushing `main` and publishing a release.
- Selected patch version `v0.9.1`. Release scope is the hardware-validated 240p AVI compatibility fix, unified selector callback, 5-second 480p/MV1C factory reset, four-button AES OSD navigation, root-menu version display, audit updates, and documentation.
- The unrelated local KiCad UI-state change in `hardware/neopico-hd/neopico-hd.kicad_prl` remains excluded. No branch or pull request; release directly from `main` per project policy.
- Bumped the single CMake project version to `0.9.1`. Rebuilt the selectable/controller MVS candidate against the exact flashed ELF baseline: hot `.scratch_x/.scratch_y`, HSTX, DMA, audio, and unified scanline symbols are unchanged; only ordinary data placement moved by 4 bytes and the audit reports no findings. Release candidate SHA-256 is `c46561c7692cf088dad8c2be732508467daf86e9fc83f1f01add37c51ab735d5`.
- Fresh GitHub-workflow-equivalent builds passed for default MVS and SNES, with embedded `NeoPico-HD v0.9.1`, the legacy 240p AVI define, and clean firmware audits. Local UF2 hashes: MVS `112399baaac03cc5ac60b752bbb4688e490c3c9062a98836b3224c3adf3a0a61`; SNES `3fe6fd7ed24486f42392ddcb1c1b9d9949fb92672c79c05a70afde6521ff4305`.
- Safe pre-commit hooks, Python syntax compilation, YAML validation, and `git diff --check` pass. The known host clang-tidy/ARM-sysroot auto-fix hook remains intentionally excluded. README release assets were corrected to the actual MVS/SNES UF2+ELF workflow output, and the opt-in AES controller build flag is documented.
- Release commit `f36ea79` (`firmware: fix 240p output and expand OSD controls`) was pushed directly to `main`. Branch Actions run `29284001910` passed MVS, SNES, and fabrication jobs.
- Created and pushed annotated tag `v0.9.1`. Tag Actions run `29284165035` passed MVS, SNES, fabrication, and release jobs, then published the non-draft release at https://github.com/fliperama86/neopico-hd/releases/tag/v0.9.1.
- Verified release assets: `neopico_hd_mvs.uf2/.elf`, `neopico_hd_snes.uf2/.elf`, and `neopico-hd-jlcpcb.zip`. Replaced the generated two-commit changelog with detailed highlights, validation, and asset notes. Actions emitted only the known Node.js 20 deprecation warnings; no job failed.

## 2026-07-13 - Experimental fake-translucent OSD
- User requested trying the dormant fake-translucent OSD technique and approved the exact implementation plan.
- Scope: compile-time flag `NEOPICO_OSD_FAKE_BLEND`, default OFF; black OSD background pixels dim captured RGB565 by a fixed 50%, while nonblack text/icons remain opaque.
- Implementation must use packed two-pixel, branch-free selection with dedicated 2x/3x/4x scratch-Y kernels; mode selection stays outside the inner loop. Reject combination with precomposed HDMI for this first test.
- Work is being ported onto current `main` without modifying the dormant old worktree. Build/audit in a separate directory before requesting a distinct hardware-flash acknowledgement.
- Validation correction: the first OSD-disabled negative configure used the default selectable-audio mode, so its existing OSD dependency failed before the new fake-blend guard. Rerun that edge case with `NEOPICO_AUDIO_MODE=DIGITAL` to exercise the intended rejection directly.
- Implemented `NEOPICO_OSD_FAKE_BLEND` default OFF. On captured OSD lines, black OSD lanes select a fixed RGB565 50% dim of the game pixel; nonblack lanes select opaque OSD color. No-source OSD rendering remains opaque.
- Linked selector candidate uses dedicated scratch-Y 2x/3x/4x kernels at `0x200810d4/0x20081148/0x200811c0`, sizes 116/120/120 bytes. Disassembly has conditional moves for lane selection and only entry/loop-control branches, with no data-dependent inner branch.
- Validation passed for the full 240p/480p/720p selector, selector without 720p, and fixed 480p/240p/720p builds. All final audits report no findings; the initial fixed-240 audit's expected flash-residency warnings were removed by validating that edge build with copy-to-RAM enabled.
- Exhaustive scalar comparison passed for every RGB565 dim value plus 250,000 randomized packed blend cases at 2x/3x/4x. Both unsupported CMake combinations, OSD disabled and precomposed HDMI, reject as intended.
- Default-OFF rebuild is byte-for-byte identical to the v0.9.1 UF2, SHA-256 `c46561c7692cf088dad8c2be732508467daf86e9fc83f1f01add37c51ab735d5`.
- Experimental controller candidate: `build-translucent-controller/src/neopico_hd.uf2`, SHA-256 `6d0388650a7b0a7b1622a80f38449302164c3a457236215c25109ddb3bd155f3`. Audit is clean with scratch-X 1664 bytes and scratch-Y 876 bytes. Not flashed.
- User acknowledged the exact experimental flash command. `pi flash build-translucent-controller/src/neopico_hd.uf2` rebooted the connected RP2350 into BOOTSEL, loaded the UF2 to 100%, and rebooted it into application mode successfully. Awaiting visual and stability verification of the fake-translucent OSD in 240p, 480p, and 720p.
- Hardware result: user reports the 50% fake-translucent OSD looks pretty good and requests a darker panel around 80% opacity.
- Proposed timing-conscious adjustment, awaiting approval: retain 18.75% of each game RGB565 channel (`1/4 - 1/16`) for effective 81.25% black-panel opacity. Keep packed two-pixel, branch-free kernels and opaque text/icons, then rebuild/audit before requesting a separate flash acknowledgement.
- User approved the opacity adjustment and states performance is the top priority, with any convenient opacity value acceptable. User also requested flashing after validation.
- Selected effective 75% black-panel opacity, retaining 25% of game RGB565 via one packed mask plus two-bit shift. This preserves the current kernel's minimal arithmetic cost and is closer to 80% than the equally cheap 87.5% option.
- Hardware coordination still requires presenting the exact rebuilt UF2 command and receiving a post-build acknowledgement before flashing it.
- Implemented the approved performance-first adjustment: black OSD background now retains 25% of each RGB565 channel, effective 75% black-panel opacity, using packed mask `0xE79CE79C` and a two-bit shift.
- Exhaustive RGB565 arithmetic and 250,000 randomized packed blend cases pass. Linked 2x/3x/4x kernel addresses and sizes remain exactly 116/120/120 bytes at the prior scratch-Y locations; disassembly still has one packed mask-plus-shift and no data-dependent inner branch.
- Final selector/controller firmware audit reports no findings with scratch-X 1664 bytes and scratch-Y 876 bytes. Default-OFF UF2 remains byte-identical to v0.9.1.
- 75% candidate: `build-translucent-controller/src/neopico_hd.uf2`, SHA-256 `da4fdb47fdb91abbdb9888dfcea8b42c42d8154a977e0950c4607274cf55a7be`. Not flashed; awaiting acknowledgement of the exact `pi flash` command.
- User acknowledged the exact 75% opacity flash command. `pi flash build-translucent-controller/src/neopico_hd.uf2` loaded the UF2 to 100% and rebooted the RP2350 into application mode successfully. Awaiting visual verification of the darker OSD and HDMI stability.
- User asks whether an equally fast but darker fake-blend value exists. Recommended effective 87.5% black-panel opacity, retaining 12.5% of game RGB565 with the same one packed mask-plus-shift instruction using a three-bit shift. A 93.75% option is equally cheap but likely too close to opaque. No change made yet.
- User approved changing the fake blend to effective 87.5% black-panel opacity. Scope remains performance-first: retain 12.5% of game RGB565 using one packed mask plus three-bit shift, with opaque OSD text/icons and no routing changes. Rebuild/audit before any separately acknowledged flash.
- Implemented 87.5% black-panel opacity by retaining one-eighth of each game RGB565 channel with packed mask `0xC718C718` and a three-bit shift.
- Exhaustive arithmetic and 250,000 randomized packed blend cases pass. Firmware audit and safe hooks pass. Linked 2x/3x/4x kernels retain the exact prior addresses, sizes, instruction count, and branch profile; only the packed mask literal and immediate shift changed.
- Default-OFF UF2 remains byte-identical to v0.9.1. Final 87.5% candidate: `build-translucent-controller/src/neopico_hd.uf2`, SHA-256 `5f55c6233a905383374d970977b7c579548f78b039bea824f64ea66a89ee3163`. Not flashed.
- User acknowledged the exact 87.5% opacity flash command. `pi flash build-translucent-controller/src/neopico_hd.uf2` loaded the UF2 to 100% and rebooted the RP2350 into application mode successfully. Awaiting visual verification of the darker panel and HDMI stability.

## 2026-07-13 - 720p Samsung Game Mode: full re-investigation for mass production (read-only)
- User goal: resolve the 720p Game Mode glitch to unblock mass production; skeptical of "just add a redriver", rejects full HDMI TX (parallel bus/GPIO cost, already ruled out 2026-06-27/07-06).
- Hard spec fact (RP2350 datasheet ch08/ch12): clk_hstx max rating is 150 MHz. 720p runs clk_hstx at 372 MHz (2.48x over spec), pads at 744 Mbps/lane vs approx 300 Mbps design envelope, through WeAct-module-to-carrier joint into the 270R resistor DAC. 480p (126 MHz) is in spec: matches "480p always clean".
- Key code finding: compile-time 720p live path AND the test-pattern path both render only every 3rd scanline (map_active_line %3 skip; test pattern early-returns identically). So the historical "test pattern + full live capture = CLEAN" test was a TIGHT A/B: Core 0 work identical, Core 1 work near-identical. Only two uncontrolled variables remain vs glitchy live: (1) Core 1 reads the shared line ring vs a hot static 640B line; (2) transmitted content is changing live video vs static bars.
- New leading hypothesis (fits every data point, never isolated): content-CHANGE transients. Every clean test transmitted statistically static frames (box/checkerboard/frozen/pattern); glitches correlate with scene cuts/flashes (button-triggered effects, MSX area transitions, KOF super flashes). A frame-wide content step changes TMDS transition-density/pad switching current at once -> rail/ground transient at a zero-margin channel -> weak direct-path receiver (Samsung Game Mode) loses decode for a moment (global corruption incl. black bars). Docs' "content-independent" reasoning only excluded static-content ISI, not steps.
- Bits-are-correct argument: Morph4K/RT4K/Normal-mode stay clean during glitch conditions, and a scaler would faithfully scale corrupted input. So the transmitted bitstream is digitally correct; the failing layer is analog margin at the sink. That is precisely the layer a redriver/level-shifter restores. Redriver skepticism is only justified if the mechanism is source clock jitter (redrivers pass jitter; retimers like TMDS181 do not).
- Untested firmware levers found: clk_hstx today follows AUXSRC=CLK_SYS (video_output_rt.c:1349, video_output.c:726), i.e. the glitchless-mux system clock shared with both loaded cores. Options: (a) AUXSRC=CLKSRC_PLL_SYS (bypass clk_sys mux/tree, one line, no cost); (b) dedicate PLL_USB to HSTX at 372 MHz (FBDIV 93 VCO 1116 PD 3x1) - datasheet ch08 explicitly endorses freeing PLL_USB for HSTX; USB survives if sysclk=336 (clk_usb = pll_sys aux /7 = 48). Also cheap: per-lane drive/slew A/Bs (e.g. gentler clock lane).
- Proposed experiment ladder (discussed, not implemented): E1 zero-build soak: static in-game screen 30+ min in Game Mode vs normal play (content-step predicts static=clean). E2 "strobe" test pattern: rewrite test_pattern_line each vsync (invert/alternate) with capture running - single-variable A/B vs the historically clean static pattern; also gives a deterministic factory/QA repro if it glitches. E3 clock-source builds (a)/(b) soaked against the repro. E4 scope only if E1-E3 inconclusive.
- Hardware menu verified: TDP158RSBT in stock at LCSC ~$2.06 (C544872, JLC-assemblable; needs 3.3V+1.1V, AC-coupled input -> add 8 series caps; symbol/footprint already drafted 2026-06-27 in neopico + pico-retrodigital). PTN3363BSMP: single 3.3V, HVQFN32, true open-drain current-steering HDMI 1.4b output + active DDC buffer, 3.4 Gbps. TMDS181 (~$4-5) only if jitter convicted. Also flagged: HDMI port currently has NO ESD protection; mass production needs it regardless of the glitch outcome.
- Recommendation delivered: run E1/E2/E3 (cheap, discriminating), and independently put a level-shifter/redriver + ESD on the production board because shipping 2.5x-overclocked resistor-DAC TMDS at 742 Mbps to the field will find more picky sinks than this one Samsung. Full report in chat; no code/hardware changed this session.
- Hardware result: user accepts the flashed 87.5% fake-translucent OSD and explicitly requests committing it.
- Commit scope is limited to the flag-gated firmware implementation, OSD documentation, audit flag, and scratchbook. Keep the unrelated KiCad `.kicad_prl` UI-state change unstaged; do not push unless requested.
