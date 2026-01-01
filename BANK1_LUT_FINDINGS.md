# Bank 1 GPIO Capture - LUT Solution & Findings
**Date:** 2026-01-01
**Status:** Correct colors achieved, minor jitter remains under investigation

---

## Executive Summary

Successfully implemented **Bank 1 GPIO video capture** on RP2350B using a **128KB lookup table** to handle non-sequential pin mapping. Colors are now correct, horizontal staircases eliminated, but minor horizontal jitter persists.

---

## Major Breakthrough: The "Staircase Mystery" Solved

### Problem
Non-sequential bit extraction caused horizontal "staircase" pattern (diagonal streaks, ~1-2 pixel drift per line):
- ✅ Sequential bit extraction → Perfect alignment, wrong colors
- ❌ Non-sequential bit extraction → Horizontal staircases, correct colors

### Root Cause: **Contiguous vs Scattered Bit Fields**

**Working version (commit fae8ed70, Bank 0):**
```c
// GPIO pins: PCLK, G4-G0, B0-B4, R0-R4
// Extraction: CONTIGUOUS fields (even if reversed)
uint8_t g_raw = (gpio_data >> 1) & 0x1F;   // Single shift+mask!
uint8_t b = (gpio_data >> 6) & 0x1F;       // Single shift+mask!
uint8_t r = (gpio_data >> 11) & 0x1F;      // Single shift+mask!
uint8_t g = green_reverse_lut[g_raw];      // 32-entry LUT for reversal
```
**Speed:** ~3 shift+mask operations total

**Our Bank 1 version (GP29-44 non-sequential):**
```c
// GPIO pins: scattered across GP31-44
// Extraction: SCATTERED bits requiring individual extraction
b = (bit6<<0) | (bit9<<1) | (bit8<<2) | (bit11<<3) | (bit10<<4);  // 5 extractions!
g = (bit7<<0) | (bit4<<1) | (bit5<<2) | (bit2<<3) | (bit3<<4);    // 5 extractions!
r = (bit13<<0) | (bit12<<1) | (bit15<<2) | (bit14<<3);            // 4 extractions!
```
**Speed:** ~14 separate bit extractions total

**Conclusion:** Scattered bit extraction is too slow, causing pixel processing to lag behind PIO capture, creating horizontal drift (staircases).

---

## Solution: 128KB Lookup Table

### Implementation
```c
// Generated at compile-time: maps all 65536 possible GPIO inputs to RGB555
static const uint16_t gpio_to_rgb555_lut[65536] = { ... };

static inline uint16_t remap_gpio_to_rgb555(uint16_t gpio_data) {
    return gpio_to_rgb555_lut[gpio_data];  // Single memory read!
}
```

### Results
- ✅ **Correct colors** - Blue text, proper backgrounds
- ✅ **No staircases** - Horizontal alignment clean
- ✅ **Fast enough** - Single memory read vs 14 bit operations
- 📊 **Binary size:** 103KB → 360KB (128KB LUT + code)

### Generation
```bash
python3 scripts/generate_gpio_lut.py
# Creates: src/video/gpio_rgb_lut.h (128KB precomputed table)
```

---

## Remaining Issue: Horizontal Jitter

### Symptoms
- **Amount:** 4-5 pixels erratic/random jumps
- **Frequency:** Continuous, all screens
- **Type:** Sudden position shifts (not smooth drift)
- **Specific case:** Logo screen (KoF98) has MORE jitter than gameplay

### Current Hypothesis
**USB frame output interference** - `fwrite()` to USB CDC may be stealing CPU cycles during critical capture window

### Testing (In Progress)
**Version with USB disabled:** Flashed at 2026-01-01 13:47
- USB frame output completely commented out
- Awaiting user confirmation on jitter behavior

### Key Questions
1. Does disabling USB eliminate/reduce jitter?
2. If yes → USB output needs optimization (background task, reduce frequency)
3. If no → Need deeper investigation:
   - LUT lookup still too slow?
   - Per-line CSYNC synchronization needed?
   - FIFO overflow/underflow?
   - Word count drift?

---

## Technical Configuration

### Pin Mapping (GP29-44)
```
Bit 0:  GP29 (PCLK) - ignored in LUT
Bit 1:  GP30 (unused)
Bit 2:  GP31 (G3)    Bit 9:  GP38 (B1)
Bit 3:  GP32 (G4)    Bit 10: GP39 (B4)
Bit 4:  GP33 (G1)    Bit 11: GP40 (B3)
Bit 5:  GP34 (G2)    Bit 12: GP41 (R1)
Bit 6:  GP35 (B0)    Bit 13: GP42 (R0)
Bit 7:  GP36 (G0)    Bit 14: GP43 (R3)
Bit 8:  GP37 (B2)    Bit 15: GP44 (R2)
```

**Missing signals:**
- R4 (GP45) - not captured, set to 0 in LUT
- CSYNC (GP46) - used for frame sync only, not captured in pixel data

### PIO Configuration
```c
PIO1 (video capture):
  GPIOBASE: 16 (window GP16-47)
  Sync SM: IN_BASE=30 (GP46 CSYNC)
  Pixel SM: IN_BASE=13 (GP29, PCLK at position 0!)
  Shift: right, autopush at 32 bits
  Wait: Absolute GPIO (wait 0 gpio 29, wait 0 gpio 46)
```

### Timing
```c
H_SKIP_START: 28 pixels (14 words)
H_SKIP_END: 36 pixels (18 words)
NEO_H_TOTAL: 384 pixels (192 words)
Active capture: 320 pixels (160 words)

Per-line FIFO clear: Enabled (drains extra words at line end)
```

---

## Comparison: Bank 0 vs Bank 1

| Aspect | Bank 0 (fae8ed70) | Bank 1 (Current) |
|--------|------------------|------------------|
| **Pins** | GP0-15 | GP29-44 |
| **Bank crossing** | No (all Bank 0) | Yes (PCLK Bank 0, data Bank 1) |
| **Pin layout** | Contiguous fields | Scattered bits |
| **Bit extraction** | 3 shift+mask ops | 128KB LUT lookup |
| **Colors** | Correct | Correct (with LUT) |
| **Stability** | Rock-solid (per user) | Minor jitter |
| **PCLK position** | Bit 0 (first) | Bit 0 (first) ✓ |

---

## Why Pin Ordering Matters

### Critical Discovery
PCLK **must be at position 0** in the capture range (like working version).

**Evidence:**
- GP31-46 capture (PCLK at end) → Diagonal staircases
- GP29-44 capture (PCLK at start) → Horizontal discontinuities (better!)
- Working version: GPIO 0 = PCLK (position 0) → Perfect

**Theory:** PIO timing or internal buffering depends on PCLK being sampled first in the `in pins, 16` operation.

---

## Files Modified

### Core Implementation
- `src/video/gpio_rgb_lut.h` - **NEW:** 128KB LUT (auto-generated)
- `scripts/generate_gpio_lut.py` - **NEW:** LUT generator
- `src/video/hardware_config.h` - Updated to use LUT
- `src/pins.h` - PIN_MVS_BASE = 29 (PCLK first)
- `src/video/video_capture.c` - Per-line FIFO clearing, diagnostics
- `src/video/video_capture.pio` - Absolute GPIO references
- `src/main.c` - USB frame output disabled (testing)

### Documentation
- `BANK1_CAPTURE_DEBUG_LOG.md` - Session 1 findings
- `BANK1_LUT_FINDINGS.md` - **This document**

---

## Next Steps

### Immediate (Awaiting User Feedback)
1. **Test USB-disabled version** - Does jitter reduce/eliminate?
2. **Check FIFO diagnostic output** - Are extra words being drained?

### If USB is the culprit
- Move USB frame output to Core 1 (background)
- Reduce frame output frequency (every 30th frame?)
- Use DMA for USB transfer instead of blocking fwrite()

### If jitter persists
- Add per-line CSYNC synchronization (like some scalers)
- Investigate FIFO overflow/underflow
- Profile LUT lookup timing
- Compare exact timing with working Bank 0 version

### Future Optimization
- Reduce LUT size: 15-bit index (skip PCLK bit) = 64KB instead of 128KB
- Or: Three 10-bit LUTs (one per color) = 3KB total
- Consider hardware rewiring for sequential pins (eliminates need for LUT)

---

## Success Criteria

- [x] ✅ Bank 1 GPIO access working (GPIOBASE=16)
- [x] ✅ Correct colors with non-sequential pins
- [x] ✅ No horizontal staircases
- [x] ✅ Readable text and stable image structure
- [ ] ⏳ Rock-solid stability (no jitter) - **IN PROGRESS**

---

## Key Learnings

1. **Contiguous bit fields >> Scattered bits** for performance
2. **LUTs are powerful** - Trade 128KB ROM for computational simplicity
3. **Pin ordering matters** - PCLK position affects capture quality
4. **Bank 1 GPIO works** - RP2350B can handle it with proper configuration
5. **USB can interfere** - Blocking I/O during capture may cause timing issues

---

*Last updated: 2026-01-01 13:50*
*Session: Autonomous debugging by Claude Code*
