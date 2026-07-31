# 720p Purple-Scanline Glitch — Investigation Tracker

**2026-07-31 resolved-profile update:** The normal runtime selector now uses exact-clock 1280x720 reduced blanking at a 64 MHz pixel clock, 320 MHz clk_hstx, 1440x741 totals, and 59.979 Hz. Historical 372 MHz and 74.4 MHz results below describe the former timing. A separately observed orange top-line artifact was identified as a not-ready capture-ring read and was removed by the default-off zero-commit frame guard. The corrected MVS release profile with exact-clock 720p and that guard enabled also fixed the rare magenta event on the previously affected Samsung. Since both changes were present, the test does not isolate which one was necessary.

> ⚠️ **2026-06-15 CORRECTION — this doc over-claimed and partially duplicated [`720P_SAMSUNG_GAME_MODE_INVESTIGATION.md`](720P_SAMSUNG_GAME_MODE_INVESTIGATION.md) (which predates it and was better calibrated). Read that one too; these should be merged.** The decisive new/old result: **test pattern + full live capture = CLEAN.** Same Core 0 load as live firmware, no glitch ⇒ **Core-0-load power/clock noise is largely refuted.** The real differentiator is **Core 1 rendering *live* captured MVS video** (reading the live line-ring while Core 0 writes it). The glitch is also **content/event-dependent** (button presses, gameplay transitions per the prior doc) — earlier "content-independent" conflated *trigger* with *effect*.

**Status:** Fixed on the affected Samsung with the 2026-07-31 exact-clock 720p plus zero-commit frame-guard profile. Historical experiments below remain useful, but their former timing is no longer main.
**Last updated:** 2026-07-31

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

## 0. 2026-07-13/15 update: content axis closed, trigger is capture-side

Context: mass-production push. Goal: either a firmware fix or a convicted mechanism that justifies the board fix.

**Spec framing (new hard fact):** the RP2350 datasheet rates **clk_hstx at 150 MHz max**. 720p runs it at **372 MHz, a 2.48x overclock** of the serializer and pads (744 Mbps/lane), through the WeAct-module-to-carrier joint into the 270R DAC. 480p (126 MHz) is in spec, which matches 480p being unconditionally clean. 720p marginality is by construction; the open question is only what consumes the last of the margin.

### New experiments (all Samsung Q80, Game Mode, direct)

| Test | Config | Result |
|---|---|---|
| E1 static-screen soak | selector firmware @720p, static in-game screen, capture + live render | **GLITCH**, >=1 in ~5 min (~0.2/min; gameplay baseline ~1/min) |
| E2a FLASH_TORTURE demo | stock `bouncing_box_rt` (Core 0 idle, no capture) + flag-gated full-field steps black -> 1px checker -> black -> white every 30 frames (~2 steps/s) | **CLEAN** (hundreds+ worst-case steps) |
| Pattern + capture soak | `build-pattern720-soak`: compile-time RT 720p, `NEOPICO_VIDEO_TEST_PATTERN=ON`, audio DIGITAL, capture fully running | **CLEAN ~10 min** (provisional: ~14% false-clean odds at 0.2/min; wants 30+ min) |

Conclusions:

- **Content change is NOT necessary** (E1: static screen glitches) and **NOT sufficient** (E2a: worst-case full-frame steps with no capture are clean). The content axis is closed; the trigger is on the capture side.
- **Statistics correction:** at the observed ~0.2/min static rate, every June ~2-min clean window (frozen frame, static pattern, checkerboard) had ~67% odds of showing zero events even if affected. Those controls are under-powered and no longer carry weight. The only robust clean baseline is the bouncing-box demo (cumulative hours).
- **Matched-pair confound found and handled:** E1 ran on the selector firmware, whose unified callback renders all 720 output lines per frame from the ring; the compile-time path (pattern build) renders every 3rd line. The sibling `build-live720-fixed` (identical to the pattern build minus TEST_PATTERN) is built and audited. Its static-screen soak is the **pending gate**:
  - live-fixed glitches: the matched pair convicts the live-ring read path (workload, content, audio, capture all controlled). Next: E3 clock builds + mechanism hunt.
  - live-fixed clean 30-45 min: compile-time 720p does not glitch at all; suspicion moves to the selector callback's 3x render duty. Fix candidate: port the every-3rd-line skip into the unified callback (a perf win regardless) and retest E1 on it.

### Bits-are-correct argument (production-relevant)

During glitch conditions the Morph4K, RT4K, and the TV's Normal mode all stay clean, and a scaler would faithfully scale corrupted input. So the transmitted bitstream is digitally correct even while the Samsung corrupts: **the failing layer is analog margin at the sink**, which is exactly the layer a redriver/level-shifter restores (see Mitigations). The one mechanism a redriver does NOT fix is source clock jitter; the E3 builds test that axis in firmware for free before any BOM decision.

### Queued firmware levers (untested)

- **E3a:** clk_hstx AUXSRC = `CLKSRC_PLL_SYS` instead of `CLK_SYS` (one line in `video_output_rt.c:1349` / `video_output.c:726`; taps the PLL output directly, bypassing the shared clk_sys glitchless mux and distribution).
- **E3b:** dedicate PLL_USB to clk_hstx at 372 MHz (FBDIV 93, VCO 1116 MHz, postdiv 3x1). The datasheet (ch08) explicitly endorses freeing PLL_USB for HSTX. USB CDC is lost at 720p unless sysclk moves to 336 MHz (then clk_usb = pll_sys/7 = 48 MHz).
- Per-lane drive/slew A/Bs (e.g. gentler clock lane; all 8 pins currently 12 mA / fast).

## 0b. Confidence & caveats (2026-06 state, read with the update above)

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
- **clk_hstx is rated 150 MHz max** (RP2350 datasheet ch08/ch12); 720p runs it at 372 MHz (2.48x over spec, AUXSRC = CLK_SYS, div 1). 480p's 126 MHz is in spec.
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

### 2026-07-13 continuation (full detail in section 0)

| # | Test | Content | Capture | Result |
|---|---|---|---|---|
| 6 | E1: selector firmware, static in-game screen | static (live ring) | running | **GLITCH ~0.2/min** |
| 7 | E2a: FLASH_TORTURE demo (flag-gated `bouncing_box_rt`) | worst-case full-frame steps | none | clean |
| 8 | `build-pattern720-soak` (pattern + capture) | static bars | running | clean ~10 min (provisional) |
| 9 | `build-live720-fixed` (matched live sibling of #8) | static (live ring) | running | **PENDING** |

---

## 6. Status of candidates (nothing is over-claimed)

Firmly ruled out:
- ❌ RT-vs-non-RT signal path (both glitch identically — strong).
- ❌ HSTX pad drive/slew as a *lever* (already at max; nothing to change).
- ❌ **Capture-data corruption** — the glitch hits black bars + OSD glyphs (no captured data). Output-side.
- ❌ **Content / ISI** — black bars (lowest-transition content) corrupt too. Content-independent.
- ❌ **Sub-line bus contention (#3)** — scanout DMA `HIGH_PRIORITY` had no effect.
- ❌ **Content-CHANGE trigger** (2026-07-13, E2a): worst-case full-frame content steps with no capture are clean, so content steps are not sufficient.
- ❌ **Content change as a required trigger** (2026-07-13, E1): a static in-game screen with capture running glitches (~0.2/min), so content change is not necessary either.
- ❌ **Transmitted-bit corruption at the source** (2026-07): scalers/Normal mode stay clean during glitch conditions and would faithfully scale bad input; the bitstream is digitally correct. The failure is analog margin at the sink.

Weakly indicated only (rests on short ~2-min clean windows; at the E1 static rate of ~0.2/min each 2-min window had ~67% false-clean odds, so re-test ≥30 min before trusting):
- ➖ HDMI output path itself (bouncing-box demo *seemed* clean).
- ➖ Displayed content / channel eye alone (frozen busy frame *seemed* clean).
- ➖ Audio/Data-Islands (demo *seemed* clean; note audio kept running during the freeze test, so it's reasonably out — but still short-window).
- ➖ Bank-1 input pin switching alone (pins live during freeze, *seemed* clean).

Partial / not actually excluded:
- ⚠️ FIFO underrun / scanout bus event — only *per-line-visible* underruns excluded; sub-line events not seen by the probe.

## 7. Working hypotheses (ranked, NONE confirmed; updated 2026-07-15)

Firm now: global link-level transient (bars corrupt), the bitstream is provably correct (scalers/Normal mode clean), it only shows on a non-re-clocking sink, the content axis is closed (E1 + E2a), and so far the glitch has appeared ONLY in builds where Core 1 renders the LIVE capture ring.

1. **Live-ring render correlation** (top by evidence, mechanism unknown): every glitching build renders the live ring; every non-ring build is clean. Digital starvation is excluded (FIFO probe, hardware ping-pong DMA chaining), so if the matched pair (`build-live720-fixed` vs `build-pattern720-soak`) confirms it, the coupling must be a physical side effect of that access/render pattern. Pending gate.
2. **Selector-callback render duty** (new 2026-07): E1 glitched on the selector firmware, whose unified callback renders all 720 lines/frame (3x the compile-time path's duty). If `build-live720-fixed` soaks clean, this becomes the lead; porting the every-3rd-line skip into the unified callback is both the test and the likely fix (and a perf win regardless).
3. **Power/EMI from capture-domain activity**: weakened (the pattern soak keeps capture fully running and looks clean so far) but not dead until the soak reaches 30+ min.
4. **Clock/timing jitter via the shared clk_sys path**: untested; E3a/E3b are cheap discriminators and shippable fixes if one works. Also the one mechanism a redriver would NOT fix.
5. **Unframed**: keep a slot open.

---

## 8. Open experiments / next steps (updated 2026-07-15)

- [x] **Baseline rates** (2026-07-13): ~1/min gameplay, ~0.2/min static screen (E1). Clean windows must now be sized against 0.2/min (30 min => <1% false-clean).
- [x] **HSTX scanout DMA `HIGH_PRIORITY` over capture DMA** — DONE 2026-06-15: **no effect** (glitch unchanged ~1/min, no new capture artifacts). ⇒ #3 out. *(change is local/uncommitted in `video_output_rt.c`; revert when done — see below.)*
- [x] **E2a content-step demo** (2026-07-13): FLASH_TORTURE `bouncing_box_rt`, clean. Content axis closed.
- [ ] **Extend `build-pattern720-soak` to 30+ min** (10 min clean so far, provisional).
- [ ] **PENDING GATE: `build-live720-fixed` static-screen soak 30-45 min.** Completes the matched pair; decides between hypotheses #1 and #2.
- [ ] **E3a**: clk_hstx AUXSRC = CLKSRC_PLL_SYS build, soak against the best repro.
- [ ] **E3b**: clk_hstx from a dedicated PLL_USB at 372 MHz, soak. (USB lost at 720p unless sysclk 336.)
- [ ] **Scope** (only if the above are inconclusive): HSTX clock-pair eye + IOVDD at the connector during a repro window.

## 9. Mitigation options (updated 2026-07-15 with parts research)

1. **Accept / scope**: ship **480p-direct** + **720p via Morph4K/RT4K**; both clean. (No cost.)
2. **Board rev, power**: better decoupling near RP2350/HSTX supply + ground return. (Cheapest hardware, but unfalsifiable across the TV population; not a mass-production bet on its own.)
3. **Board rev, TMDS level shifter / redriver**: restores exactly the layer that is failing (analog margin at the sink; the bitstream is proven correct). Zero GPIO, zero firmware, pin-strap parts:
   - **NXP PTN3363BSMP**: single 3.3V, HVQFN32, true open-drain current-steering HDMI 1.4b-compliant output up to 3.4 Gbps, integrated active DDC buffer. Cleanest fit for this rate class.
   - **TI TDP158RSBT**: 6 Gbps, proven in Xbox One X; ~$2.06 at LCSC (C544872, JLC-assemblable). Needs a 1.1V rail plus AC-coupling caps on the inputs. KiCad symbol/footprint already drafted 2026-06-27 (here and in pico-retrodigital).
   - Caveat: a redriver does NOT remove source clock jitter. Run E3a/E3b first; if a clock build fixes the glitch, jitter was the mechanism and the redriver is then margin insurance rather than the fix.
   - **TI TMDS181** (~$4-6): full retimer with jitter cleaning; only needed if E3 convicts jitter that PLL isolation cannot fix.
4. **ESD protection on the HDMI port**: currently absent entirely; required for mass production regardless of the glitch outcome (hot-plug and static exposure in the field).
5. **Architecture change**: FPGA + real HDMI transmitter (see References). Out of scope; = rebuilding the project. A full HDMI TX chip fed by the RP2350 also stays rejected: the 24-bit parallel bus does not fit the pin/bandwidth budget (2026-06-27 / 2026-07-06 analyses).

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

## Test build recipes (2026-07-13 experiments)

```bash
# E2a FLASH_TORTURE demo (full-field content steps, Core 0 idle).
# Gated by a CMake option in the example; OFF build is byte-identical to stock.
cmake -S lib/pico_hdmi/examples/bouncing_box_rt -B build-flashdemo -DFLASH_TORTURE=ON
cmake --build build-flashdemo -j12   # -> build-flashdemo/bouncing_box_rt.uf2

# Pattern + capture soak (compile-time RT 720p; audio must be a fixed mode
# because SELECTABLE requires the resolution selector):
cmake -S . -B build-pattern720-soak -DPICO_SDK_PATH=$HOME/pico-sdk \
  -DPICO_BOARD=weact_studio_rp2350b_core \
  -DNEOPICO_VIDEO_720P=ON -DNEOPICO_VIDEO_TEST_PATTERN=ON \
  -DNEOPICO_USE_NONRT_HDMI=OFF -DNEOPICO_AUDIO_MODE=DIGITAL \
  -DNEOPICO_RESOLUTION_MENU=OFF -DNEOPICO_RESOLUTION_MENU_720P=OFF \
  -DNEOPICO_OSD_RES_CONFIRM=OFF -DNEOPICO_FIRST_BOOT_REBOOT=OFF

# Matched live sibling (identical minus the pattern; the pending gate):
#   same flags without -DNEOPICO_VIDEO_TEST_PATTERN=ON, build dir build-live720-fixed
```
