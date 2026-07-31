# 720p Samsung Game Mode Investigation

## Summary

NeoPico-HD 720p output previously showed split-second video glitches on at
least one Samsung TV when Game Mode was enabled. The corrected 2026-07-31 MVS
test profile is now clean on that affected TV.

The current evidence points away from a generic PicoHDMI 720p signal problem and
toward a live-capture sensitivity that Samsung Game Mode exposes more readily
than other sinks.

## Observed Behavior

- 480p NeoPico-HD output is stable.
- 720p PicoHDMI bouncing-box firmware is stable on the Samsung TV in Game Mode.
- 720p NeoPico-HD is stable on tested high-end gaming monitors.
- 720p NeoPico-HD is stable on the Samsung TV when Game Mode is disabled.
- 720p NeoPico-HD can glitch on the Samsung TV when Game Mode is enabled.
- The glitch was initially correlated with controller button presses, but later
  testing showed it can also happen without button presses during specific game
  activity, such as a Metal Slug X area transition.

## Tests Performed

### RT vs Non-RT HDMI

Both 720p non-RT and 720p RT NeoPico-HD builds showed similar behavior on the
Samsung TV in Game Mode.

Conclusion: the issue is not specific to the non-RT PicoHDMI command-list path.

### PicoHDMI Bouncing-Box 720p

The standalone PicoHDMI 720p bouncing-box demo was stable on the Samsung TV in
Game Mode.

Conclusion: the Samsung TV can accept the RP2350/PicoHDMI 720p timing when the
output is generated internally and does not depend on live MVS capture.

### Capture Freeze

A 720p non-RT NeoPico-HD build captured one MVS frame, stopped the capture
PIO/DMA path, and continued outputting the frozen frame.

Result: no glitches were observed, including while pressing controller buttons.

Conclusion: the HDMI output path and the frozen captured frame are stable. The
problem requires live capture/rendering activity.

### Static Test Pattern With Live Capture Running

A 720p non-RT build left live MVS capture running but rendered a static internal
test pattern instead of captured video.

Result: no glitches were observed.

Conclusion: live capture workload alone is not enough to disturb HDMI output.
The issue appears only when the HDMI renderer consumes live captured MVS video.

### Completed-Frame Latch Candidate

A test build latched only fully captured frames and used a larger 512-line ring.
This was intended to avoid presenting an in-progress capture frame to the HDMI
renderer.

Result: not conclusive. Later testing still suggested content-dependent glitches
around specific gameplay transitions.

Conclusion: this remains a plausible mitigation area, but it is not proven as a
complete fix.

## Current Working Theory

The most likely model is:

1. Some MVS activity occasionally causes a transient capture-side disturbance.
2. The disturbance affects captured line/frame data, CSYNC/PCLK timing, or frame
   handoff.
3. Most displays tolerate, buffer, or hide the transient.
4. Samsung Game Mode exposes it because it is lower-latency and less processed.

This does not currently look like a plain HDMI signal-integrity failure, because
the same 720p HDMI output path is stable with the bouncing-box demo, frozen
capture, and internal test pattern.

## Why 720p Can Expose It

Capture and output are logically separate, but they are not physically
independent:

- 720p runs the RP2350 at a higher clock rate and drives HSTX harder.
- 720p increases memory and DMA pressure compared with 480p.
- 720p presents each source line at 3x vertical scale, so a bad captured line is
  visually amplified.
- Samsung Game Mode likely has less buffering than normal TV modes.

The 720p mode may therefore make an existing capture-side marginality visible
without being the root cause.

## Next Useful Debug Steps

The next work should focus on capture integrity rather than more HDMI mode
switching:

1. Instrument capture health counters for sync resets, missed line readiness,
   ring underruns, and frame publication timing.
2. Probe MVS CSYNC and PCLK near the RP2350 input pins during content that
   triggers the glitch.
3. Compare the same probes during stable attract-mode scenes and unstable
   gameplay transitions.
4. Re-test completed-frame latching only after adding diagnostics, so the result
   can be correlated with capture events.
5. If electrical disturbance is confirmed, investigate input conditioning,
   grounding, cable routing, and isolation around CSYNC/PCLK and the video bus.

## 2026-07 Update

A second investigation round (2026-07-13, driven by the mass-production
decision) added three experiments and revised the picture. Full detail and the
living log now live in [`720P_PURPLE_GLITCH.md`](720P_PURPLE_GLITCH.md); the
summary:

- **E1, static-screen soak**: the selector firmware at 720p glitched on a
  static in-game screen (at least one event in ~5 minutes, versus roughly one
  per minute during play). Content change is therefore not required.
- **E2a, flash-torture demo**: the stock bouncing-box demo, modified only to
  step the whole frame between black, a 1-pixel checkerboard, and white every
  30 frames (about two worst-case content steps per second, Core 0 idle),
  stayed clean. Content steps are therefore not sufficient either.
- **Pattern plus capture soak**: the static-test-pattern-with-live-capture
  configuration from this document's original round was re-run for ~10 minutes
  and stayed clean (provisional; the observed static-screen rate of ~0.2/min
  means ~14% odds of a false clean at 10 minutes).

Consequences for this document's conclusions:

- The observed static rate retroactively weakens every ~2-minute clean window
  reported above: each had roughly two-in-three odds of showing nothing even
  if affected. Only the bouncing-box demo cleanliness (cumulative hours)
  remains load-bearing.
- The transmitted bitstream is provably correct during glitch conditions
  (scalers and the TV's Normal mode stay clean and would faithfully display
  corrupted input), so the failure is analog margin at the sink, not data.
- The surviving correlation is that only builds where Core 1 renders the live
  capture ring have ever glitched. A matched pair to convict or clear that
  variable is built and pending: `build-live720-fixed` (live render) versus
  `build-pattern720-soak` (pattern render), identical in all else.
- Relevant hardware context established: the RP2350 datasheet rates clk_hstx
  at 150 MHz; 720p runs it at 372 MHz, so 720p output is a 2.48x overclock of
  the HSTX serializer and pads by construction. 480p is in spec.

## Status

Main now uses an exact-clock 64 MHz reduced-blanking runtime 720p descriptor at
59.979 Hz instead of the former 74.4 MHz, 372 MHz-HSTX timing. The standalone
PicoHDMI bouncing-box version passed an initial smoke test on the picky Samsung.
The timing is non-CTA, advertises VIC 0 with explicit 16:9, and may be less
compatible with other TVs, so 720p remains Experimental.

A NeoPico test then exposed a different top-of-frame artifact. Changing only
the missing/not-ready capture-line fallback from gray to International Orange
made that artifact orange, directly identifying a capture-ring readiness miss.
USB readiness logging observed not-written events but also disturbed the image,
so the counts are qualitative. A default-off frame guard that retains the
previous complete frame when the newest frame has committed zero lines looked
clean in an initial no-logging test.

The combined exact-clock plus frame-guard profile fixed the rare magenta
Samsung Game Mode transient on the affected TV. These changes were tested
together, so the result does not prove whether clock/timing, frame selection,
or their combination was necessary. The frame guard remains separately useful
because the orange diagnostic directly identified a not-ready ring read.

The historical experiment queue and mechanism analysis are maintained in
[`720P_PURPLE_GLITCH.md`](720P_PURPLE_GLITCH.md).
