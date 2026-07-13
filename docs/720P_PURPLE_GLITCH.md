# 720p Purple-Scanline Glitch — Investigation Tracker

> ⚠️ **2026-06-15 CORRECTION — this doc over-claimed and partially duplicated [`720P_SAMSUNG_GAME_MODE_INVESTIGATION.md`](720P_SAMSUNG_GAME_MODE_INVESTIGATION.md) (which predates it and was better calibrated). Read that one too; these should be merged.** The decisive new/old result: **test pattern + full live capture = CLEAN.** Same Core 0 load as live firmware, no glitch ⇒ **Core-0-load power/clock noise is largely refuted.** The real differentiator is **Core 1 rendering *live* captured MVS video** (reading the live line-ring while Core 0 writes it). The glitch is also **content/event-dependent** (button presses, gameplay transitions per the prior doc) — earlier "content-independent" conflated *trigger* with *effect*.

**Status:** 🟠 NOT solved — direction only, "strongly suggests." Differentiator = rendering live captured video (not Core 0 load alone). Leading: **capture-side timing/handoff disturbance** (CSYNC/PCLK, or in-progress line-ring read race) amplified at 720p, exposed by Game Mode. Power/clock signal-integrity **demoted** (test-pattern refutes). Need: long timestamped baseline + event correlation, then scope **CSYNC/PCLK** + capture-health counters.
**Last updated:** 2026-06-15

> ⚠️ **Localization UNRESOLVED (2026-06-15).** Eyeballing a split-second glitch's spatial extent is unreliable. Observations conflicted within a single run (sometimes content-only, sometimes bars too), so we CANNOT yet say content-side vs global. Both stay open. **To resolve: film a glitch in slow-mo and inspect the frame** — bars affected? and is the corruption a horizontal *tear* (→ ring race) vs uniform *noise/color-shift* (→ bad captured line / CSYNC-PCLK)?
>
> **⚠️ Photo-reading is unreliable — DO NOT conclude mechanism from TV photos.**
> Across high-fps paused frames (2026-06-15) the assistant flip-flopped content↔global 3-4x; #3 was misread as "OSD clean" then corrected to **entire frame incl. OSD glitched**; camera moiré can mimic "diagonal hatching." Spatial extent (content-only vs global) is **UNRESOLVED** and not trustworthy from photos.
>
> **The ONE hard, controlled constraint (not photo-based):** the glitch requires **Core 1 rendering LIVE captured video**. bouncing-box / checkerboard / capture-freeze / full-capture-with-static-test-pattern = clean; only live-video render glitches. Both "captured-data wrong" and "live-render triggers an output-composition disturbance" fit this; photos can't separate them.

> **Capture-health counters (NEOPICO_DIAG_COUNTERS, USB-serial, 2026-06-15):** instrumented line-ring readiness (NOTWR), overrun (OVR), capture sync-loss (SYNCRST), and in/out frame rates. Across two clean windows (~160 s + ~145 s) of glitchy play with confirmed glitches: **ALL FLAT** (NOTWR frozen at startup value, OVR=0, SYNCRST=0, no frame drops). ⇒ **capture-timing / line-ring readiness / sync-loss / handoff is very likely NOT the cause** — lines are delivered on-time and "ready" through glitches. (Caveat: limited glitch sample/window; USB-CDC stalls at ~2.5 min at the 372 MHz OC. Counters keep accumulating in firmware regardless.)
>
> **Remaining fork (neither visible to these counters):**
> (a) **Wrong DATA in an on-time line** — capture *sampling* error (PCLK phase / RGB-bus) → wrong bits → LUT emits wrong colors, line delivered normally. Would NOT trip SYNCRST (no full signal loss).
> (b) **Output-side** — scaler/HSTX corrupts after the ring.
> Next discriminator: a high-contrast **solid local reference block** overlaid on live capture, frame-stepped during a glitch — if the *local* block corrupts → output-side (b); if only the captured area → data-side (a). (Cleaner than the OSD text, which was misread.)
>
> **Line-ring torn-read race:** note ARM 32-bit aligned writes are atomic → a torn read mixes whole valid pixels of near-identical frames (near-invisible), so it's a weak fit for wrong-*color* garbage — but not formally excluded.
>
> **Path forward must be instrumented, not visual:** capture-health counters (sync resets, line-readiness, ring state, frame timing) correlated to glitches; controlled long-window experiments; scope CSYNC/PCLK + RGB input bus + output. Stop ranking from images.

## 0. Confidence & caveats (read first)

- **Baseline rate (2026-06-15): ~1 glitch/min** on live 720p Game Mode. This makes comparisons easier to judge: a 2-min clean window expects ~2 events, so seeing zero is *reasonably* meaningful (not decisive — a 10-min confirm is better). It also retro-strengthens the Core-0-correlation tests (freeze/demo/checkerboard clean).
- **Still: under-power earlier "clean" results.** Each was watched only ~2 min. Suggestive, not proof; re-run load-bearing ones for ≥10-20 min before fully trusting.
- **"Not a FIFO underrun" is partial.** The probe samples once per scanline, so it only excludes *per-line-visible* underruns; a sub-line scanout/bus event would not show as FIFO<7 or a GAP spike.
- **Competing hypotheses still alive** (none excluded): (a) Core 0 power/EMI coupling into the eye; (b) Core 0 perturbing HSTX *clock*/timing jitter (Game-Mode sinks are jitter-sensitive; we free-run 720p at a non-standard 60.11 Hz); (c) a sub-line bus/scanout event; (d) unframed.
- **Sink-side fact is solid** (reproduced, user has seen it before): glitches only on a **direct/non-re-clocking** path (TV Game Mode); any buffered sink (Normal mode, Morph4K, RT4K) hides it. This constrains *where it shows*, not *what causes it* on our side.
- **Output-side confirmed (2026-06-15, direct observation — not under-powered):** with the OSD open, the glitch corrupts the **whole frame including the black pillarbox bars** (pure locally-generated constant black, zero captured data, lowest-transition content). ⇒ capture-data corruption is OUT, and content/ISI is OUT (black is the *easiest* pattern to transmit, yet it corrupts). It is a **content-independent global transient** on the TMDS output.
**Owner:** dudu

> Living document. Append to the Investigation Log as new tests are run; update Status + Current Conclusion at the top when the picture changes.

---

## 1. Summary (current conclusion)

At **720p only**, on a **Samsung Q80 4K TV connected directly in Game Mode**, a rare (every ~30 s–2 min) split-second burst of **purple/green scanline TMDS corruption** appears over the image. It does **not** occur in the TV's Normal mode, via Morph4K/RT4K, or at 480p.

Two things look reasonably firm: (1) **sink-side**, it only shows on a **direct/non-re-clocking** path (every clean case is a sink that buffers/re-clocks); (2) it **correlates with Core 0 running the capture pipeline** (frozen-frame test was clean over a short window). What we have **not** nailed is the **on-board mechanism** — power/EMI vs clock jitter vs a sub-line bus event are all still in play (see §0). Earlier framing of this as "root cause = Core 0 EMI, confirmed" was over-stated; the clean tests behind it were too short to be decisive.

---

## 2. Symptom

- Brief (split-second) purple/magenta + green scanline corruption overlaid on the correct image.
- Frequency: irregular, ~30 s to ~2 min apart.
- Self-recovers immediately; no sync loss, no crash.
- Reference capture: KOF98, see chat image 2026-06-15.

## 3. Environment / repro

| Factor | Glitches? | Sink re-clocks? |
|---|---|---|
| 720p, Samsung Q80 direct, **Game Mode** | ✅ yes | no (direct, low-latency) |
| 720p, Samsung Q80 direct, **Normal mode** | ❌ no | yes (processing pipeline) |
| 720p, via **Morph4K** | ❌ no | yes (scaler buffer) |
| 720p, via **RT4K** | ❌ no | yes (scaler buffer) |
| 480p, Samsung Q80 direct (any mode) | ❌ no | n/a — bigger eye |
| 720p, RT path firmware | ✅ yes | (Game Mode) |
| 720p, **non-RT** path firmware | ✅ yes (same) | (Game Mode) |

> Pattern: glitches **iff** the sink consumes the stream **directly without re-clocking** (Game Mode). Any buffered/re-clocking path (Normal mode, Morph4K, RT4K) hides it — same as a buffered receiver riding over a marginal eye.

## 4. Board / signal facts

- HDMI output is the **PicoDVI-style 270 Ω resistor-DAC** off HSTX GPIO 12-19 (8 pins → 4 TMDS pairs). DDC (SDA/SCL) + HPD present. **No redriver, no ESD array, no level-shifter.** (Confirmed in `hardware/neopico-hd/neopico-hd.kicad_sch`.)
- This is a marginal pseudo-differential TMDS approximation; eye quality degrades as pixel clock rises.
- Per-lane bit rate: **720p ≈ 742 Mbps** vs **480p ≈ 252 Mbps** (~3×).
- HSTX pads already at max: `GPIO_DRIVE_STRENGTH_12MA` + `GPIO_SLEW_RATE_FAST`.

---

## 5. Investigation log

### Isolation ladder (all at 720p)

| # | Test | Content | Core 0 | Result | Conclusion |
|---|---|---|---|---|---|
| 1 | non-RT 720p firmware | game | capturing | **GLITCH** | not RT-specific |
| 2 | `bouncing_box_rt` demo | flat (box) | idle | clean | HDMI-out path OK |
| 3 | checkerboard demo (8 px B/W) | busy | idle | clean | channel passes busy content |
| 4 | firmware + `CAPTURE_FREEZE_AFTER_FRAME` | **real busy frame** | **stopped** | clean | **content/channel exonerated** |
| 5 | live firmware | real busy | capturing | **GLITCH** | **Core 0 capture is the variable** |

Same busy content on screen (test 4 vs 5): frozen = clean, live = glitch ⇒ it is the capture **work**, not the pixels displayed.

### FIFO / timing probe (`PICO_HDMI_PERF_PROBE`)

- Added a Self Test readout of HSTX FIFO-min + max IRQ gap per 1 s window.
- During glitches: **FIFO steady at 7/8, GAP ~23 µs (~1 line).** No underrun, no IRQ stall.
- ⇒ **Not** a scanout-DMA starvation / bus-contention underrun (rules out the rp2350-doom mechanism; BUSCTRL bank-placement would not help).

---

## 6. Status of candidates (nothing is over-claimed)

Firmly ruled out:
- ❌ RT-vs-non-RT signal path (both glitch identically — strong).
- ❌ HSTX pad drive/slew as a *lever* (already at max; nothing to change).
- ❌ **Capture-data corruption** — the glitch hits black bars + OSD glyphs (no captured data). Output-side.
- ❌ **Content / ISI** — black bars (lowest-transition content) corrupt too. Content-independent.
- ❌ **Sub-line bus contention (#3)** — scanout DMA `HIGH_PRIORITY` had no effect.

Weakly indicated only (rests on short ~2-min clean windows — re-test ≥15-20 min before trusting):
- ➖ HDMI output path itself (bouncing-box demo *seemed* clean).
- ➖ Displayed content / channel eye alone (frozen busy frame *seemed* clean).
- ➖ Audio/Data-Islands (demo *seemed* clean; note audio kept running during the freeze test, so it's reasonably out — but still short-window).
- ➖ Bank-1 input pin switching alone (pins live during freeze, *seemed* clean).

Partial / not actually excluded:
- ⚠️ FIFO underrun / scanout bus event — only *per-line-visible* underruns excluded; sub-line events not seen by the probe.

## 7. Working hypotheses (ranked, NONE confirmed)

Firm now: it's a **content-independent global TMDS transient** (black bars corrupt), it correlates with **Core 0 capture running**, and it only shows on a **non-re-clocking sink**. The on-board mechanism is undecided — but both survivors are *global* disturbances, which fits "even black corrupts":

1. **Power/EMI** — Core 0 burst current (DMA + 256 KB LUT) → rail/ground noise → couples into the marginal 720p eye. Fits "FIFO clean," unproven.
2. **Clock/timing jitter** — Core 0 perturbs HSTX clock/timing; a re-clocking sink hides it, Game Mode doesn't. The DMA-priority test would NOT address this.
3. ~~**Sub-line bus/scanout event**~~ — **DISFAVORED (2026-06-15):** marking scanout DMA `HIGH_PRIORITY` over capture DMA had **no effect** (glitch unchanged at ~1/min). If capture DMA were starving scanout, priority would have helped. Effectively out.
4. **Unframed** — keep a slot open.

⇒ Leading candidates are now **#1 (power/EMI)** and **#2 (clock jitter)** — both electrical, both need a **scope** to separate, neither cleanly firmware-fixable.

---

## 8. Open experiments / next steps

- [ ] **FIRST: establish a trustworthy repro + baseline.** Watch live 720p Game Mode and log glitch timestamps for ~20-30 min to get a real rate; only then are "clean" comparisons meaningful. (All prior clean results are ~2-min and under-powered.)
- [x] **HSTX scanout DMA `HIGH_PRIORITY` over capture DMA** — DONE 2026-06-15: **no effect** (glitch unchanged ~1/min, no new capture artifacts). ⇒ #3 out. *(change is local/uncommitted in `video_output_rt.c`; revert when done — see below.)*
- [ ] **Scope** (the real disambiguator): HSTX output eye + 3V3 rail ripple + clock jitter, Core-0-idle vs capturing. Separates #1 vs #2 directly.
- [ ] (If #1) reduce Core 0 di/dt — spread DMA/LUT bursts, LUT placement. *(speculative)*

## 9. Mitigation options

1. **Accept / scope** — ship **480p-direct** + **720p via Morph4K/RT4K**; both clean. (No cost.)
2. **Board rev — power** — better decoupling near RP2350/HSTX supply + ground return. (Cheapest hardware.)
3. **Board rev — TMDS redriver** — re-levels HSTX TMDS to spec; bigger eye tolerates the noise. Keeps RP2350/HSTX architecture. (Real fix, moderate.)
4. **Architecture change** — FPGA + real HDMI transmitter (see References). Out of scope; = rebuilding the project.

## 10. References

- `cps2_digiav` (`~/Projects/references`) — Neo Geo digital → HDMI via **ADV7513** real HDMI transmitter (parallel video in, spec TMDS out). Flawless 720p on any sink, but FPGA-based; cannot bolt onto RP2350-HSTX.
- Memory: `project_720p_purple_glitch.md` (mirrors this doc, cross-session).
- Distinct from the **other** 720p issue: `project_720p_xip_overclock_crash` (hard-crash from XIP-at-overclock, fixed by `copy_to_ram`).

---

## Test build recipes (worktree `nonrt-720p-test`)

```bash
# FIFO probe on Self Test (RT 720p, genlock-off):
cmake ... -DNEOPICO_VIDEO_720P=ON -DNEOPICO_USE_NONRT_HDMI=OFF \
  -DNEOPICO_RESOLUTION_MENU=OFF -DNEOPICO_RESOLUTION_MENU_720P=OFF \
  -DNEOPICO_OSD_RES_CONFIRM=OFF -DNEOPICO_FIRST_BOOT_REBOOT=OFF
#   + src/CMakeLists.txt: target_compile_definitions(pico_hdmi PRIVATE PICO_HDMI_PERF_PROBE=1)

# The one-frame capture-freeze isolation flag used during this investigation
# was removed after the capture path was ruled out. Use NEOPICO_DIAG_COUNTERS
# or NEOPICO_VIDEO_TEST_PATTERN for current capture/output isolation work.

# Torture/checkerboard demo: lib/pico_hdmi/examples/bouncing_box_rt + #define TORTURE_PATTERN
```
