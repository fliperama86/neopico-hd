# How SHADOW and DARK Work on Neo Geo

## Firmware Note (Current Build)

NeoPico-HD currently runs a performance-first conversion path:

- Default build (`NEOPICO_ENABLE_DARK_SHADOW=OFF`) uses RGB-only conversion.
- Optional build (`NEOPICO_ENABLE_DARK_SHADOW=ON`) uses a 128KB quantized LUT path: normal RGB555 is exact; DARK/SHADOW effects use 14-bit color indices.
- In that optional mode, DARK applies the fixed dark offset.
- SHADOW halves channels before expansion and forces DARK active.

The sections below describe the reference behavior of Neo Geo effects.

## The Basic Idea

The Neo Geo outputs **15-bit color** (5 bits each for Red, Green, Blue), giving 32 levels per channel (0-31).

But the hardware has **two extra signals** that modify brightness *after* the color is generated.

---

## SHADOW (50% Dimming)

**What it does:** Cuts the brightness **in half**.

```
Before:  Bright red pixel = R:31, G:0, B:0
After:   Shadowed red     = R:15, G:0, B:0
```

**How:** Each color channel is divided by 2 (right-shift by 1 bit).

**Used for:**
- Character shadows on the ground
- Darkened background layers
- "Behind something" effect

**Visual analogy:** Like putting sunglasses on just that pixel.

---

## DARK (Subtle Dimming)

**What it does:** Makes colors **slightly darker** - subtracts a small fixed amount.

```
Before:  R:200, G:150, B:100  (after 5-bit to 8-bit expansion)
After:   R:196, G:146, B:96   (subtract 4 from each)
```

**How:** Subtract 4 from each 8-bit channel (with floor at 0, so black stays black).

**Used for:**
- Subtle shading effects
- Smooth fade transitions
- Fine intensity control

**Visual analogy:** Like turning down a dimmer switch just a tiny bit (~1.5% darker).

---

## When Both Are Active (Reference)

Reference behavior treats SHADOW and DARK as **independent controls**:

```
Original pixel:        R:31, G:31, B:31  (white)
                             |
                             v
After SHADOW (÷2):     R:15, G:15, B:15  (gray)
                             |
                             v
After DARK (-4):       R:11, G:11, B:11  (darker gray)
```

**Result:** Roughly **60% darker** than the original.

Current staged firmware note: output conversion is keyed by both SHADOW and DARK in the optional LUT mode.

---

## Visual Summary

```
NORMAL pixel:   ████████████████  (100% brightness)

DARK pixel:     ███████████████   (~98% - barely noticeable)

SHADOW pixel:   ████████          (50% - obvious dimming)

SHADOW+DARK:    ██████            (~48% - darkest)
```

---

## Why Two Separate Signals?

The Neo Geo designers wanted:

1. **SHADOW:** Cheap way to create shadow sprites without storing separate darker graphics
2. **DARK:** Fine control for smooth fade-in/fade-out effects

Both are **per-pixel** flags, so different parts of the screen can have different dimming applied simultaneously.
