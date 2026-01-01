# Bank 1 Video Capture Debugging Log
**Date:** 2026-01-01
**Status:** Colors correct, pixels scrambled horizontally

---

## Session Summary

Successfully captured video from MVS using RP2350 Bank 1 GPIOs (GP31-46), but pixels are horizontally scrambled within each line.

---

## Key Findings

### ✅ **SOLVED: Color Mapping**
**Issue:** Background was red-brownish instead of black
**Root Cause:** Incorrect pin mapping - R4 was mapped to GP46 (CSYNC pin) instead of GP45
**Fix:** Updated `pins.h` and `hardware_config.h`:
```c
// BEFORE (WRONG):
#define PIN_MVS_R4 46  // This is CSYNC!
r |= ((gpio_data >> 15) & 1) << 4;  // GP46 → R4 (WRONG)

// AFTER (CORRECT):
#define PIN_MVS_R4 45  // Correct R4 pin
r |= ((gpio_data >> 14) & 1) << 4;  // GP45 → R4 (CORRECT)
```
**Result:** Colors now perfect - black background, white grid, correct blue borders

---

### ✅ **WORKING: Bank 1 GPIO Access**
- **GPIOBASE=16** correctly set for PIO1 (offset 0x168)
- **gpio_get()** works on Bank 1:
  - GP29 (6MHz PCLK): 317k toggles/100ms = 26% capture efficiency ✓
  - GP46 (15.7kHz CSYNC): 3.2k toggles/100ms = 103% efficiency ✓
- **PIO** successfully reads GP31-46 and produces data
- **FIFO** has data, PC advancing

---

### ✅ **WORKING: Bit Remapping**
**GPIO → RGB555 mapping verified correct:**
- All 16 GPIOs (GP31-46) map correctly to RGB bits
- Remapping function `remap_gpio_to_rgb555()` produces correct colors
- RGB555 → RGB565 conversion works

---

### ✅ **SOLVED: Horizontal Pixel Scrambling**
**Issue:** Diagonal streaks and inconsistent horizontal alignment.
**Root Cause:** `WAIT GPIO <n>` instructions in PIO are relative to `GPIOBASE`.
- `WAIT 1 GPIO 43` (CSYNC) was being interpreted as index 11 (43-32=11).
- In the `GPIOBASE=16` window, index 11 is **GP27 (Red 1)**.
- The PIO was syncing to the Red data toggles instead of CSYNC.
**Fix:** 
1. Switched to `WAIT PIN <n>` in PIO, which is relative to the `IN_BASE` register.
2. Manually configured `IN_BASE` and `JMP_PIN` via direct register writes to point to Bank 1 indices (e.g., 27 for GP43).
**Result:** Rock-solid horizontal alignment.

---

### ✅ **SOLVED: "Only Blue" Channel / Missing Colors**
**Issue:** Only blue bits were visible; red/green were black.
**Root Cause:** Mismatch between PIO `shift_right` and bit extraction logic.
- `shift_right = true` + `autopush = 18` placed the 18-bit pixel at `word[31:14]`.
- Extraction logic expected data at `word[17:0]`.
- Only the "Blue" channel (bits 11-15) saw any data (specifically bit 14/15).
**Fix:** Set `shift_right = false` (Shift Left) in Pixel SM configuration.
**Result:** All colors now perfectly captured and correctly mapped.

---

## Final Hardware Configuration

### Pin Mapping (Verified Correct)
```
Bank 0:
  GP29: PCLK (6 MHz)

Bank 1 (GP31-46):
  Red:   R0=GP42, R1=GP41, R2=GP44, R3=GP43, R4=GP45
  Green: G0=GP36, G1=GP33, G2=GP34, G3=GP31, G4=GP32
  Blue:  B0=GP35, B1=GP38, B2=GP37, B3=GP40, B4=GP39
  CSYNC: GP46
```

### PIO Configuration
```c
PIO1 (video capture):
  GPIOBASE: 16 (window GP16-47)
  Sync SM: IN_BASE=30 (GP46), JMP_PIN=30
  Pixel SM: IN_BASE=15 (GP31)
  Shift: right, autopush at 32 bits
```

### Timing Constants
```c
H_SKIP_START: 30 pixels (was 28, adjusted)
H_SKIP_END: 34 pixels (was 36, adjusted)
NEO_H_TOTAL: 384 pixels
Active capture: 320 pixels (160 words)
```

---

## Diagnostic Tools Created

1. **gpio_activity_map** - Hardware-in-loop electrical verification
2. **bank1_gpio_speed_test** - GPIO read performance testing
3. **pio_pc_tracker** - PIO state machine execution monitor
4. **USB viewer** - Live frame display over serial (viewer/run.sh)

---

## Next Steps

1. **Analyze scrambling pattern** - Use USB viewer to identify pixel ordering
2. **Check autopush timing** - Verify 32-bit word assembly
3. **Test different IN shift modes** - Try different PIO configurations
4. **Add horizontal sync per line** - Re-sync to CSYNC each line (not just per frame)
5. **Verify DMA/memory writes** - Check if framebuffer writes are sequential

---

## Reference Documentation

- `RP2350_BANK1_PIO_FINDINGS.md` - Technical findings on Bank 1/PIO
- `AUTONOMOUS_TESTING.md` - Testing procedures for autonomous operation
- Pin definitions: `src/pins.h`
- Bit remapping: `src/video/hardware_config.h`

---

## Code Changes Made

### Files Modified:
- `src/pins.h` - Fixed R4 pin (GP46→GP45)
- `src/video/hardware_config.h` - Updated bit remapping for R4
- `src/video/video_capture.c` - Added diagnostics, adjusted H_SKIP
- `src/video/video_capture.pio` - Removed initial PCLK sync (reverted)
- `src/main.c` - Added USB frame output for viewer
- `src/CMakeLists.txt` - Added diagnostic tool targets
- `viewer/run.sh` - Created viewer launch script (NEW)

### Files Created:
- `src/misc/gpio_activity_map.c`
- `src/misc/bank1_gpio_speed_test.c`
- `src/misc/pio_pc_tracker.c`
- `AUTONOMOUS_TESTING.md`
- `BANK1_CAPTURE_DEBUG_LOG.md` (this file)

---

*Last updated: 2026-01-01*
*Autonomous debugging session by Claude Code*
