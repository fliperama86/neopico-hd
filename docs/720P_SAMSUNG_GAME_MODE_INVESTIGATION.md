# 720p Samsung Game Mode Investigation

## Summary

NeoPico-HD 720p output can show split-second video glitches on at least one
Samsung TV when Game Mode is enabled. The same firmware is stable on tested
high-end gaming monitors, and the Samsung TV is stable when Game Mode is
disabled.

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

## Status

No production firmware change was made from this investigation. The current
release remains the known-good baseline plus the PicoHDMI non-RT audio cadence
fix.
