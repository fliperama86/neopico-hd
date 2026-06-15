# NeoPico-HD — Live State

**Contract:** this document is LIVE — edited in place to always reflect current
reality. Any agent or human picking up the project starts here. The
append-only history stays in `SCRATCHBOOK.md`; architecture rules in
`AGENTS.md`; protocol details in `docs/`.

_Last updated: 2026-06-12 (late evening; 720p precomposed genlock — signal
measurably perfect, picky-sink suspected, second-TV test is the gate)._

---

## TL;DR — where we are right now

- **S1 (480p precomposed-on-RT) is VALIDATED** (run #9, ~1 h clean). The
  runtime-modes RT library running the *precomposed tiny ISR* is the
  architecture of record. The copy-model ISR is on death row.
- **S3 (720p precomposed) is implemented and mostly working.** Picture is
  correct (pillarboxed 4:3, 3x scale), audio works, RS=0 (no HDMI desync
  ever), FIFO never underflows, ISR never runs late. Four real defects were
  found and fixed today (see ledger). A **dynamic genlock** (locks output to
  the MVS ~59.186 Hz via vtotal + sub-line elastic-blanking trim) was built
  from scratch.
- **One symptom remains:** once the genlock LOCKS, the sink shows a
  whole-frame jolt (OSD + game jump together) every ~8–20 s. **Every on-chip
  signal we can measure is constant through these jolts** (FIFO level, ISR
  gap, vtotal, trim, audio-underrun count). The unlocked/drifting state is
  always clean.
- **Prime remaining suspect: the TV.** Locked output is non-CEA 720p (762
  lines, 59.186 Hz, ~1.4% off standard). Same TV ran 480p (standard 525-line)
  clean for an hour. **THE NEXT ACTION IS A ONE-CABLE TEST ON A SECOND
  DISPLAY** — no code. It splits the world: second sink clean → ship as a
  per-sink quirk; second sink also jolts → it's on the wire in a dimension we
  haven't instrumented yet.

**Live build (uncommitted, on the bench):** `build-720p-s3/`. Boots with the
OSD open on a new **Genlock** telemetry screen. All work is in the working
tree of BOTH repos — nothing committed yet (see "Uncommitted code" below).

---

## Current bench build: `build-720p-s3/`

Flags: `NEOPICO_VIDEO_720P=ON NEOPICO_EXP_PRECOMPOSED_HDMI=ON
NEOPICO_EXP_GENLOCK_DYNAMIC=ON NEOPICO_ENABLE_OSD=ON NEOPICO_ENABLE_SELFTEST=ON
NEOPICO_OSD_ROOT_MENU=ON NEOPICO_COPY_TO_RAM=ON NEOPICO_VIDEO_DVI_ONLY=OFF`
(`PICO_HDMI_RUNTIME_MODES=ON` — this is the **RT** path).

Flash: `pi flash build-720p-s3/src/neopico_hd.uf2` (auto reboot-to-bootloader;
the board occasionally drops off USB and needs a cable reseat / BOOT-reset —
this happened ~3x today, not a firmware issue).

### Genlock OSD screen (boots open; also root-menu entry "Genlock")
Row layout (what to read on the bench):
- `PHASE` — µs between MVS vsync and HDMI vsync. Setpoint target = 11000.
  Healthy: parks near 11000 and wanders slowly within the deadband.
- `TRIM` — applied elastic-blanking trim in pixel-clocks (negative = shorter
  blanking = faster output). Equilibrium ≈ −5…−7 (was −17…−20 before the
  uniform-trim fix). Constant for minutes at a time.
- `SLOTS` — # of blanking-line RAW_REPEAT words being trimmed. Should be 10
  (7 clean templates + 3 DI templates). If 0, the servo is a no-op.
- `VTOTAL` — parked at 762 in steady state. 761/763 only flicker during
  acquire.
- `UPTIME` — seconds since boot (doubles as a glitch stopwatch).
- bottom probe row `F.. G.. U.....`:
  - `F` = min HSTX FIFO level seen in last 1 s (≈7, never dips at glitch).
  - `G` = max inter-IRQ gap µs in last 1 s (≈23, never spikes at glitch).
  - `U` = cumulative audio silence-packet insertions (climbs fast at boot to
    ~24000 then frozen; no glitch correlation).

---

## 720p GENLOCK SAGA — investigation ledger (2026-06-12)

Goal: lock the 720p output frame rate to the MVS (~59.186 Hz) so the
free-run beat doesn't cause periodic gray frames, AND do it without
disturbing the sink. The MVS is the timing master; we slave HDMI to it.

**Defects found & fixed, in order (each had a distinct on-screen fingerprint):**

1. **Free-run beat → full gray frame ~1/s** (run #10). Output free-ran 60.00
   Hz vs MVS 59.185; once per ~1.3 s beat the HDMI vsync sampled a capture
   frame-base with zero lines committed → whole frame prepped as no-signal
   gray. *Fix:* dynamic genlock (slave output rate to MVS).

2. **Top-of-image disruption (3-4x band duplication)** — ring lapping. The
   16-line pre-expanded ring holds ~1.07 ms of beam; the Core 1 background
   task legitimately stalls 100s of ms (audio SRC bursts), so prep fell
   behind and the beam wrapped onto stale entries. *Fix:* moved prep off the
   background task onto a **hardware alarm IRQ on Core 1** (250 µs tick,
   bounded ~6 lines/tick, preempts audio bursts; scanline DMA ISR still
   preempts it). `video_pipeline_p720_alarm_cb`.

3. **Frame-restart race** — vsync ISR reset the prep cursor while the alarm
   IRQ was mid-prep; the post-increment landed after the reset → line 0
   skipped → stale top slots. *Fix:* vsync only bumps `p720_frame_seq`; the
   alarm context is the SOLE owner of the prep cursors and detects the new
   frame itself (single writer).

4. **Top garbage under stretched vtotal — LATENT RT LIBRARY BUG.**
   `get_scanline_state()` classified active video as
   `!vsync && !front_porch && !back_porch`, but `active_line` was computed as
   `v_scanline - (v_total - v_active)`. When genlock stretches `v_total`, the
   extra lines between back porch and active got classified active with a
   huge/negative `active_line` → garbage scanout at the top. *Fix:* active
   video is now PINNED right after the back porch
   (`blank_head = v_fp + v_sync + v_bp`), so vsync-to-active distance never
   depends on v_total and stretched lines land at the frame BOTTOM (invisible
   blanking). **This bug affected every dynamic-genlock experiment ever run on
   this lib, including 480p/532-line.** Fixed in lib + mirrored to 2.1-dev.

5. **Gradual gray fills screen every ~4 s — wrong nominal.** 760-line nominal
   was computed for the CEA 74.25 MHz pixel clock, but the hardware runs
   **372 MHz / 5 = 74.4 MHz** (NOT 74.25). At 74.4 MHz, 760 lines = 59.33 Hz
   (too fast) so the phase wrapped. *Fix:* `GENLOCK_NOMINAL_VTOTAL_720 = 762`
   (74.4M / (1650·762) = 59.187 Hz, +2 µs/frame vs MVS — near-perfect),
   plus a fast-acquire state.

6. **Glitch cycle (6 s / 66 s, then 8 s / 23 s) with horizontal kicks — the
   genlock CONTROLLER itself.** This was the long fight. Progression:
   - vtotal bang-bang (762↔761) dithered the frame period a whole line
     (~22 µs) every few frames → sink V-PLL hunts. **Finding: this sink
     visibly reacts to vtotal steps.**
   - → **Elastic blanking (sub-line trim):** keep vtotal CONSTANT, trim a few
     pixels off the big RAW_REPEAT in blanking lines (~0.5 µs/frame per pixel,
     44x finer than a line, below sink line-PLL perception). NEW 2.1 LIBRARY
     FEATURE: `video_output_set_vblank_htrim_px()`. This is the genlock's
     fine actuator.
   - servo sign bug (positive feedback), then limit-cycling (trim/drift/phase
     is a double integration), then a deadlock in the outlier rejection
     (stale reference rejected every real sample → trim pinned at clamp).
     Each fixed in turn: outlier reject w/ streak-accept, slew limit
     1 px/frame, then a **slow integrator** (hold trim constant; step ±1 px
     only past a ±400 µs deadband, rate-limited), then **derivative gating**
     (only step when phase is not already returning — anti-windup + damping).
   - **Bench finding that reframed everything:** trim sitting at the −30 clamp
     (CONSTANT) → clean; trim at the servo equilibrium hunting ±1 px → glitch.
     The sink reacts to trim ACTIVITY, not value.

7. **Intra-vblank sawtooth — uniform trim fix.** We were trimming only ~7 of
   the ~42 blanking lines (DI-carrying templates were excluded out of
   caution). That imposed a line-to-line H-period sawtooth (1650 vs 1650+trim)
   inside every vblank, which the sink tracked once |trim| > ~14 px (matches
   "glitches start around −14"). *Fix:* register the DI vblank templates too
   (`vblank_di_ping/pong/null`). False positives impossible: TERC4 payload
   words always carry high bits, can't match a bare RAW_REPEAT in the
   1200–1600 px window. Tripled authority → equilibrium trim −17 → ~−5.

**After all 7 fixes: the residual jolt.** Locked = jolt every ~8–20 s,
unlocked = clean, AT ANY SETPOINT (tested 8000 and 11000). Discriminators run:
- OSD-jumps-with-picture test → **whole output frame**, not a capture slip.
- FIFO min `F` + ISR-gap `G` probe → **constant through jolts** (output
  delivery provably perfect; no underflow, no late ISR).
- vtotal-spike debounce + raw-excursion counter `A` → `A` stuck at boot value
  → **no measurement-spike-induced vtotal steps**.
- audio silence-insert counter `U` → climbs at boot then frozen, **no glitch
  correlation** → audio queue exonerated.
- DVI-only discriminator → **INVALID / no signal**: precomposed templates
  carry HDMI guard-band/preamble structure, can't be served as clean DVI.
  (Lib gap to note for 2.1; do not retry as-is.)

**Conclusion / current hypothesis:** every on-chip signal dimension is
measurably CONSTANT while the sink jolts. The one thing unique to the locked
state is a frozen, non-CEA timing (762 lines, 59.186 Hz, ~1.4% off standard).
Leading theory: **the TV's format-detection / re-sync heuristic twitches on a
near-but-not-standard timing it can't fully settle on.** Supporting: this same
TV ran standard-timing 480p (525 lines) clean for an hour.

### >>> NEXT ACTION (no code): test a SECOND display <<<
One cable swap. `build-720p-s3` as-is.
- Second sink clean at lock → genlock is correct; this TV dislikes near-CEA
  timing. Treat as per-sink quirk (consider a future "standard-timing +
  accept-the-beat" fallback mode, or true CEA 74.25 via a different
  sysclk/divider).
- Second sink jolts identically → it IS on the wire; instrument the last
  un-probed dimensions (data-island TERC4 content / IEC channel-status
  cadence / infoframe checksum timing under lock).

---

## Uncommitted code (BOTH repos dirty — nothing committed for the 720p work)

**`~/Projects/neogeo/neopico-hd`** (working tree):
- `src/video/video_pipeline.c` — 720p precomposed path (`p720_*` ring, alarm
  IRQ prep, retry pass) + the entire genlock servo (`genlock_dynamic_update`,
  elastic-trim integrator with deadband + derivative gating + slew limit +
  vtotal-spike debounce). Constants: `GENLOCK_NOMINAL_VTOTAL_720=762`,
  `GENLOCK_PHASE_SETPOINT_US=11000` (was 8000; an EXPERIMENT value — revisit),
  `PULLBACK=14000`, `RESUME=4000`, `P720_PREP_INTERVAL_US=250`,
  `P720_RING_LINES=16`. Publishes `g_genlock_phase_us`,
  `g_genlock_outzone_count`.
- `src/experiments/menu_diag_experiment.c` — new `MENU_SCREEN_GENLOCK` +
  "Genlock" root entry; boots OSD open on it; 1 Hz value updates; probe row.
- `src/osd/selftest_layout.c` — reverted the temporary debug line (clean).
- `src/main.c`, `src/video/video_pipeline.h` — `video_pipeline_precomp_background`
  hook + 720p decls.
- `src/CMakeLists.txt` — precomposed 720p restriction relaxed (240p still
  FATAL_ERROR: "S3b pre-doubled ring not done").
- `src/video/scale_pixels.S` (NEW) + `NEOPICO_TRIPLE_ASM` flag (default OFF) —
  hand-written M33 kernels for the 3x/4x pixel-scale (the alarm-IRQ hot path):
  STM-batched stores + post-inc loads, ~1.8-2x vs C, in `.scratch_y` (RAM).
  Boot `video_pipeline_scale_selftest()` compares asm vs C refs over several
  counts (incl. odd/zero); verdict in `g_scale_asm_selftest_ok`, shown on the
  Genlock OSD as `ASM:OK`/`ASM:BAD`. Host-simulated register-faithful (ALL
  MATCH) + disassembly-verified; NOT yet run on device. `build-720p-s3/` is
  currently configured with `NEOPICO_TRIPLE_ASM=ON` (built, not flashed).
  NOTE: this is a prep-MARGIN win, NOT a fix for the locked-state jolt (that's
  still the second-TV question). Flag-OFF is empty-object equivalent by
  construction (`#if NEOPICO_TRIPLE_ASM` guards the whole .S; C bodies under
  `#if !NEOPICO_TRIPLE_ASM`).
- `lib/pico_hdmi` (submodule pointer M; the nested checkout is itself dirty).

**`~/Projects/pico_hdmi`** (branch `2.1-dev`, `src/video_output_rt.c` dirty):
- Elastic blanking: `video_output_set_vblank_htrim_px()` +
  `_get_vblank_htrim_slots/px()` + `htrim_register_all()` (hooked after
  `compose_ring_built`). **2.1 feature.**
- Active-video pinned after back porch (defect #4 fix). **2.1 fix.**
- Perf probe: `video_output_perf_probe_read()` (FIFO min + IRQ gap).
- NOTE: the neopico nested `lib/pico_hdmi` copy and the standalone
  `~/Projects/pico_hdmi` 2.1-dev tree must be kept in sync by hand; the
  active-line fix is mirrored, but verify the htrim + probe code matches
  before committing/pushing 2.1-dev.

**Commit plan when the second-TV verdict is in:** (1) commit lib 2.1-dev
(elastic blanking + active-pin fix + probe) and push branch; (2) commit
neopico 720p path + genlock + Genlock OSD screen behind the existing flags
(verify flag-OFF UF2 unchanged); (3) bump submodule pointer.

---

## Resolved foundation (earlier today — context for the above)

- **Precomposed-on-RT (S1) works.** The exact RT+features path that dropped
  sync in seconds as the *copy-model* ISR is rock-solid as the *precomposed
  tiny ISR* (run #9). RS=0 all day across every 720p experiment confirms the
  RT machinery itself never faulted.
- **XIP layout sensitivity → SOLVED:** `NEOPICO_COPY_TO_RAM=ON`
  (pico_set_binary_type) eliminates runtime XIP; this is why dead code used to
  perturb sync. The precomposed tiny-ISR is the only ISR light enough to also
  survive copy_to_ram (the heavyweight copy-model RT ISR + striped-SRAM fetch
  contention was TOXIC — dropped in seconds).
- **`__scratch_x("")` merges ALL functions into one section** → the linker
  can never GC dead scratch code individually; #if-gate unused scratch
  functions out (did this for the copy-model dispatcher; scratch_x 0x800→0x5c0).
- Canonical 480p baseline: run #6, sha `76c1233d`,
  `build-precomp-baseline/`. RT×copy_to_ram TOXIC policy stands. Scenario B
  (precomposed → runtime-modes path = pico_hdmi 2.1) ratified and in progress.

## Remaining 2.1 stages (DESIGN-2.1.md)
- **S2** — swap-free DMA topology (eliminate the per-IRQ 16/32-bit `al1_ctrl`
  swap). NOTE: 720p uses 32-bit pointer mode and never touches the swap, so
  the S2 hazard is ABSENT at 720p; it's a 480p/native-mode concern.
- **S3b** — 240p (4x = pre-doubled ring + hardware 2x). 720p (3x) done.
- **S4** — runtime mode switching + wire the Resolution menu entry onto the
  precomposed path (currently reboot-based).

## Run log (one line per soak)

| # | Date | Firmware (sha + flags) | Outcome |
|---|------|------------------------|---------|
| 1 | 06-11 | v0.6.0 release | clean >1 h |
| 2 | 06-11 | 981efab+3d1cb91, flags OFF | clean ~1 h |
| 3-5,7 | 06-11/12 | precomp+OSD+selftest, flash/XIP | sync drops (XIP, since fixed) |
| 6 | 06-12 | pure phase-2 baseline 76c1233d | CLEAN ~8 h overnight |
| 8 | 06-12 | oracle: glitch-patch + COPY_TO_RAM | CLEAN — XIP convicted |
| 9 | 06-12 | **S1** RT+precomp+OSD+menu+copy_to_ram (`build-s1/`) | **CLEAN ~1 h — S1 validated** |
| 10 | 06-12 | S3-720p first cut | gray flash ~1/s; RS=0 (free-run beat) |
| 11-20 | 06-12 | S3-720p + genlock iterations (`build-720p-s3/`) | 7 defects found & fixed; see ledger |
| 21 | 06-12 | S3-720p genlock final (setpoint 11000, uniform trim, probes) | locked picture clean EXCEPT whole-frame jolt ~8-20 s; all on-chip signals constant → **second-TV test pending** |

## Standing constraints (digest; full rules in AGENTS.md + SCRATCHBOOK)
- scratch_x hard boundary 0x800; measure before adding anything to scratch
  (`grep -A1 '^\.scratch_x' src/neopico_hd.elf.map`). Live build: ~0x6b0.
- `__scratch_x("")` functions can't be GC'd individually — #if-gate dead ones.
- No printf/blocking I/O on Core 1 background path (1 UART line = 7 ms stall).
- OSD render pattern: full draw on entry, value-only ~1 Hz updates.
- Flag-gate everything experimental, default OFF; verify flag-OFF UF2 byte-identical.
- Validate audio with a pure sine + spectrogram (SFX/square waves mask drops).
- The neopico `lib/pico_hdmi` checkout is intentionally a separate dirty tree;
  keep it in sync with `~/Projects/pico_hdmi` 2.1-dev by hand.
