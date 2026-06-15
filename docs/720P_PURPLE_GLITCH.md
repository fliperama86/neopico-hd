# 720p Purple-Scanline Glitch — Investigation Tracker

**Status:** 🟡 NOT concluded. Leading hypothesis (signal integrity exposed by direct/unbuffered sinks) fits the observations, but the supporting "clean" tests are statistically under-powered (see §0). Multiple mechanisms still open. Do not anchor.
**Last updated:** 2026-06-15

## 0. Confidence & caveats (read first)

- **Under-powered "clean" results.** Each clean observation (bouncing box, checkerboard, capture-freeze) was watched for only **~2 min**, against a glitch whose period is itself **up to ~2 min**. A single 2-min clean window proves little — a condition that *does* glitch can pass it by luck. Treat every "clean" below as *suggestive, not decisive*. Re-run any load-bearing one for **≥15–20 min** before trusting it.
- **"Not a FIFO underrun" is partial.** The probe samples once per scanline, so it only excludes *per-line-visible* underruns; a sub-line scanout/bus event would not show as FIFO<7 or a GAP spike.
- **Competing hypotheses still alive** (none excluded): (a) Core 0 power/EMI coupling into the eye; (b) Core 0 perturbing HSTX *clock*/timing jitter (Game-Mode sinks are jitter-sensitive; we free-run 720p at a non-standard 60.11 Hz); (c) a sub-line bus/scanout event; (d) unframed.
- **Sink-side fact is solid** (reproduced, user has seen it before): glitches only on a **direct/non-re-clocking** path (TV Game Mode); any buffered sink (Normal mode, Morph4K, RT4K) hides it. This constrains *where it shows*, not *what causes it* on our side.
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

Weakly indicated only (rests on short ~2-min clean windows — re-test ≥15-20 min before trusting):
- ➖ HDMI output path itself (bouncing-box demo *seemed* clean).
- ➖ Displayed content / channel eye alone (frozen busy frame *seemed* clean).
- ➖ Audio/Data-Islands (demo *seemed* clean; note audio kept running during the freeze test, so it's reasonably out — but still short-window).
- ➖ Bank-1 input pin switching alone (pins live during freeze, *seemed* clean).

Partial / not actually excluded:
- ⚠️ FIFO underrun / scanout bus event — only *per-line-visible* underruns excluded; sub-line events not seen by the probe.

## 7. Working hypotheses (ranked, NONE confirmed)

Two observations look firm: it correlates with **Core 0 capture running**, and it only shows on a **non-re-clocking sink**. The on-board mechanism is undecided:

1. **Power/EMI** — Core 0 burst current (DMA + 256 KB LUT) → rail/ground noise → couples into the marginal 720p eye. Fits "FIFO clean," unproven.
2. **Clock/timing jitter** — Core 0 perturbs HSTX clock/timing; a re-clocking sink hides it, Game Mode doesn't. The DMA-priority test would NOT address this.
3. **Sub-line bus/scanout event** — a brief scanout stall the per-line probe can't see.
4. **Unframed** — keep a slot open.

---

## 8. Open experiments / next steps

- [ ] **FIRST: establish a trustworthy repro + baseline.** Watch live 720p Game Mode and log glitch timestamps for ~20-30 min to get a real rate; only then are "clean" comparisons meaningful. (All prior clean results are ~2-min and under-powered.)
- [ ] **HSTX scanout DMA `HIGH_PRIORITY` over capture DMA** — tests hypothesis #3 (sub-line bus). Helps → bus; no change → weakly against #3 (needs the longer baseline to mean anything).
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
  -DNEOPICO_EXP_REBOOT_MODE_SWITCH=OFF -DNEOPICO_EXP_REBOOT_MODE_SWITCH_720P=OFF \
  -DNEOPICO_OSD_RES_CONFIRM=OFF -DNEOPICO_EXP_FIRST_BOOT_REBOOT=OFF
#   + src/CMakeLists.txt: target_compile_definitions(pico_hdmi PRIVATE PICO_HDMI_PERF_PROBE=1)

# Capture-freeze (real busy frame, Core 0 stopped):
cmake ... -DNEOPICO_VIDEO_720P=ON -DNEOPICO_USE_NONRT_HDMI=OFF \
  -DNEOPICO_CAPTURE_FREEZE_AFTER_FRAME=ON  (+ same OSD/reboot OFF flags)

# Torture/checkerboard demo: lib/pico_hdmi/examples/bouncing_box_rt + #define TORTURE_PATTERN
```
