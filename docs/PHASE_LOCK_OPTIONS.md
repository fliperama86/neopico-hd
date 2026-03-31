# Phase Mismatch Mitigation Options

## Purpose

Document practical options to reduce latency pressure caused by source/output phase drift, while preserving stability as the top priority.
The core problem is that Neo Geo capture timing and HDMI output timing are in different clock domains. Even a small frequency mismatch causes phase walk over time. Large buffers hide this drift; small buffers expose it.

## Scope and Priority

- Top priority: stability.
- Secondary priority: lower latency through smaller safe buffer depth.
- Default behavior should remain conservative and production-safe.

## Dual Firmware Timing Strategy

To avoid mixing incompatible goals in one default behavior, ship two explicit firmware profiles:

- **Compat60 firmware**: strict 60 Hz compatibility-focused behavior for broad sink support.
- **SourceLock firmware**: source-matched behavior for drift reduction/latency experimentation.

Recommended policy:

- Compat60 remains the production fallback.
- SourceLock is opt-in and validated with soak tests before any buffer-depth reduction.

## Baseline Facts

- Current stable baseline: 256-line ring buffer, dynamic low-latency controllers disabled.
- 256 lines correspond to roughly one frame-class elasticity window (~16 ms at ~63.5 us/line).
- Small-buffer modes (128/40) become unstable unless drift is tightly controlled.
- Current frame-level VTOTAL chase approaches can help temporarily but can also enter limit cycles.

## Constraint Summary (RP2350 / HSTX / HDMI Path)

- HSTX output path is highly timing-sensitive.
- Existing implementation assumes fixed line structure and strict per-line timing.
- Runtime VTOTAL modulation is possible and cheap, but coarse (frame-level +/-1 style adjustments).
- Aggressive runtime corrections can destabilize image behavior on reduced headroom.
- Sink behavior is not identical across displays/scalers; compatibility must be treated as a hard constraint.

## Option 1: Keep Conservative Production Baseline

Description:

- Keep 256-line ring and no active VTOTAL phase controller.
Pros:
- Highest known stability.
- Best sink compatibility confidence.
- Lowest implementation risk.
Cons:
- Higher latency than desired.
- Does not reduce drift; only absorbs it.
When to use:
- Production default and fallback mode.

## Option 2: Fixed Pre-Tuned Output Rate

Description:

- Use a preselected output timing target near observed source cadence.
Pros:
- Simple implementation.
- Lower risk than dynamic per-frame control.
Cons:
- Residual mismatch remains due to unit-to-unit and environment variation.
- May be insufficient for aggressive low-buffer operation.
When to use:
- Controlled hardware population with narrow clock variance.

## Option 3: Boot-Time Source Measurement + Static Trim

Description:

- Measure source cadence during boot.
- Apply one static output trim based on measured cadence.
- Optional very slow background re-measure with strict hysteresis.
Pros:
- Good balance of drift reduction and stability.
- Avoids fast control-loop oscillation.
- Better adaptation than fixed pre-tune.
Cons:
- Still not true lock.
- Requires careful measurement quality and outlier filtering.
When to use:
- First software path for balanced mode.

## Option 4: Runtime Drift Trim (Very Slow, Guarded)

Description:

- Keep runtime control, but only as long-horizon drift correction (not frame-by-frame chase).
- Use strict guard rails, cooldown, and fallback.
Pros:
- Can track slow drift changes without hardware changes.
Cons:
- More complex and riskier than static trim.
- If too aggressive, can reintroduce limit-cycle behavior.
When to use:
- Experimental mode only, behind feature flags.

## Option 5: True Hardware-Reference Lock via XIN (Board-Level)

Description:

- Feed MVS master clock into RP2350 XIN in bypass mode.
- Derive output timing from the shared reference domain.
Pros:
- Strongest long-term drift suppression.
- Best candidate for stable low-latency operation at smaller buffers.
Cons:
- Requires hardware design changes and validation.
- Signal conditioning, jitter, and electrical compatibility must be verified.
- XOUT is not used as an active driven output in bypass reference mode.
When to use:
- Next hardware revision path for best long-term result.

## Option 6: Full Dynamic Genlock Chasing VSYNC Phase

Description:

- Per-frame output modulation to chase measured phase error directly.
Pros:
- No hardware changes.
- Can reduce drift in some conditions.
Cons:
- Coarse actuator and delayed response can oscillate.
- Historically prone to unstable cycling under low headroom in this project.
When to use:
- Research only, not production default.

## Decision Matrix (Stability-First)

- Production today: Option 1.
- Best near-term software candidate: Option 3.
- If Option 3 passes long-run tests, cautiously evaluate Option 4.
- Best long-term low-latency architecture: Option 5.
- Option 6 should remain experimental due to oscillation risk.
- Delivery model: separate firmware images for Compat60 and SourceLock.

## Recommended Staged Path

1. Keep conservative production baseline unchanged.
2. Implement boot-time measurement + static trim behind new flag (default OFF).
3. Validate at 256-line buffer first with long-run soak tests.
4. If stable, reduce ring depth stepwise (224 -> 192 -> 160 -> 128), one variable at a time.
5. Maintain immediate fallback to production baseline.
6. Track hardware-genlock feasibility for next PCB revision.

## Test/Acceptance Gates

For any non-baseline mode:

- No gray-frame cycling.
- No partial-frame corruption.
- No sink handshake regressions.
- No audio regression.
- Stable operation over extended soak windows.
Failing any gate reverts mode to conservative baseline.
