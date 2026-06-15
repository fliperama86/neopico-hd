# 720p Purple-Scanline Glitch — Investigation Tracker

**Status:** 🟡 Root cause identified (Core 0 capture activity) · mechanism = likely power/EMI · one firmware tiebreaker experiment still open
**Last updated:** 2026-06-15
**Owner:** dudu

> Living document. Append to the Investigation Log as new tests are run; update Status + Current Conclusion at the top when the picture changes.

---

## 1. Summary (current conclusion)

At **720p only**, on a **Samsung Q80 4K TV connected directly**, a rare (every ~30 s–2 min) split-second burst of **purple/green scanline TMDS corruption** appears over the image. It is **caused by Core 0's active capture work**, not by the displayed content and not by a FIFO underrun. Leading mechanism: Core 0 burst current draw injecting power/ground noise into the marginal 720p resistor-DAC TMDS eye. **480p-direct is clean; 720p via Morph4K is clean.**

---

## 2. Symptom

- Brief (split-second) purple/magenta + green scanline corruption overlaid on the correct image.
- Frequency: irregular, ~30 s to ~2 min apart.
- Self-recovers immediately; no sync loss, no crash.
- Reference capture: KOF98, see chat image 2026-06-15.

## 3. Environment / repro

| Factor | Glitches? |
|---|---|
| 720p, Samsung Q80, **direct** | ✅ yes |
| 480p, Samsung Q80, direct | ❌ no |
| 720p, via **Morph4K** | ❌ no |
| 720p, RT path firmware | ✅ yes |
| 720p, **non-RT** path firmware | ✅ yes (same) |

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

## 6. Ruled out

- ❌ RT-vs-non-RT signal path (both glitch identically).
- ❌ HDMI output path itself (bouncing-box demo clean).
- ❌ Displayed content / channel eye alone (frozen real busy frame clean).
- ❌ FIFO underrun / scanout bus starvation (probe clean through glitches).
- ❌ Audio/Data-Islands (demo has audio islands and is clean).
- ❌ HSTX pad drive/slew (already maxed).
- ❌ Bank-1 input pin switching alone (pins still electrically live during freeze, yet clean).

## 7. Current root cause + mechanism

**Root cause:** Core 0's active capture pipeline (PIO1 sampling + capture DMA + the interp0 **256 KB LUT** RGB conversion).

**Leading mechanism:** since the scanout FIFO/timing stays clean, the corruption is downstream of the FIFO — most consistent with **Core 0 burst current draw → power-rail/ground noise → coupling into the already-marginal 720p TMDS eye**, occasionally tipping the strict Q80 receiver past its error threshold. 480p's larger eye absorbs the same noise; the Morph4K re-clocks its input.

---

## 8. Open experiments / next steps

- [ ] **HSTX scanout DMA `HIGH_PRIORITY` over capture DMA** — tiebreaker for sub-line bus micro-stall (which the per-line probe could miss) vs power/EMI. If it kills the glitch → firmware-fixable; if not → confirmed electrical. *(cheap, ~2 min)*
- [ ] (If electrical) measure with a scope: HSTX output eye + 3V3 rail ripple, idle vs capturing.
- [ ] (If power) try reducing Core 0 di/dt — spread DMA/LUT bursts, LUT placement. *(speculative)*

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
