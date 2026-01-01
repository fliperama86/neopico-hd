# GP25-42 Capture Configuration - Current Status

**Date:** 2026-01-01
**Status:** Partial capture working - Blue channel only

---

## Configuration Summary

### Pin Mapping (GP25-42, 18-bit capture)
```
GP25: PCLK (position 0) ✓
GP26-30: Red (R0-R4) - 5 bits contiguous
GP31-35: Green (G0-G4) - 5 bits contiguous
GP36-40: Blue (B0-B4) - 5 bits contiguous
GP41: DARK (not captured yet)
GP42: SHADOW (not captured yet)
GP43: CSYNC (outside capture window) ✓
```

### PIO Configuration
- **GPIOBASE:** 16 (PIO window GP16-47)
- **IN_BASE:** 9 (GP25 at index 9)
- **Capture:** `in pins, 18` (GP25-42)
- **Autopush:** 18 bits (1 pixel per FIFO word)

### Bit Extraction (hardware_config.h)
```c
uint8_t r = (gpio_data >> 1) & 0x1F;   // Bits 1-5: Red (GP26-30)
uint8_t g = (gpio_data >> 6) & 0x1F;   // Bits 6-10: Green (GP31-35)
uint8_t b = (gpio_data >> 11) & 0x1F;  // Bits 11-15: Blue (GP36-40)
```

---

## What's Working ✓

1. **HDMI/DVI Output:** Verified with color bar test pattern - all channels working
2. **18-bit PIO Capture:** Successfully capturing data from GP25-42
3. **Blue Channel:** Extracting and displaying correctly
4. **PCLK Sync:** Positioned at GP25 (bit 0) - no jitter observed
5. **Frame Sync:** CSYNC at GP43 working correctly
6. **Fast Extraction:** Contiguous bit fields - no LUT overhead needed

---

## Current Issue ❌

**Only Blue channel visible in captured frames.**

### Evidence
- USB frame capture shows only blue tones
- Character sprites visible but monochrome blue
- Red and Green channels appear as zero/black

### Wiring Verified by User
- Red signals (R0-R4) connected to GP26-30 ✓
- Green signals (G0-G4) connected to GP31-35 ✓
- Blue signals (B0-B4) connected to GP36-40 ✓
- PCLK at GP25 ✓
- CSYNC at GP43 ✓

---

## Possible Causes

### 1. Capture Path Issue
- Red/Green data not being captured by PIO despite wiring
- Bits 1-5 and 6-10 of `gpio_data` reading as zero

### 2. Wiring Contact Issue
- Dupont wires for Red/Green not making good contact
- Only Blue channel has solid connection

### 3. MVS Signal Issue
- MVS hardware not outputting Red/Green on those specific pins
- Blue working suggests MVS is alive, but partial output

### 4. Bit Position Mismatch
- Extraction positions might be incorrect
- Blue works by coincidence at different position

---

## Next Steps

### Immediate Diagnostics

1. **Check HDMI Display During MVS Capture**
   - Does HDMI show full color game graphics?
   - Or only blue like USB capture?
   - This determines if issue is capture or display

2. **Raw GPIO Data Dump**
   - Print raw `gpio_data` values to USB serial
   - Check if bits 1-5 (Red) and 6-10 (Green) have any data
   - Verify bit positions align with wiring

3. **Test Pattern Injection**
   - Temporarily bypass capture and fill framebuffer with known RGB values
   - Confirms HDMI can display all channels (already verified with color bars)

### Hardware Verification

4. **Dupont Wire Contact Test**
   - Wiggle Red/Green wires while observing capture
   - Check for intermittent connection
   - Try swapping Blue wire position with Red to verify Blue wiring is good

5. **MVS Output Verification**
   - Use multimeter/scope to verify MVS is outputting on R/G pins
   - Check voltage levels on GP26-30 (Red) and GP31-35 (Green)
   - Compare with GP36-40 (Blue) which is working

### Code Verification

6. **Swap Channel Assignments**
   - Temporarily extract bits 11-15 as Red (instead of Blue)
   - If Red appears instead of Blue → extraction code is correct, wiring is wrong
   - If still Blue → extraction positions are wrong

7. **Enable Debug Output**
   - Re-enable printf statements in capture code
   - Print first few pixels of each line with bit breakdown
   - Verify FIFO data matches expectations

---

## Key Files

- **`src/pins.h`** - Pin definitions (GP25-42 layout)
- **`src/video/hardware_config.h`** - Bit extraction (`extract_rgb555_contiguous()`)
- **`src/video/video_capture.c`** - PIO configuration, capture loop
- **`src/video/video_capture.pio`** - 18-bit capture PIO program
- **`src/main.c`** - Main loop, framebuffer initialization
- **`src/misc/test_patterns.c`** - Color bar test (verified working)

---

## Performance Notes

- **No jitter** with contiguous extraction (vs LUT version)
- **1 pixel per FIFO word** (18-bit capture)
- **320 FIFO reads/line** (vs 160 with 16-bit/2-pixel mode)
- Performance acceptable - no visible lag

---

## Success Criteria

- [x] ✅ GP25-42 capture range working
- [x] ✅ PCLK at position 0 (no timing issues)
- [x] ✅ Contiguous bit field extraction (fast)
- [x] ✅ HDMI output verified (color bars)
- [ ] ⏳ **All three RGB channels capturing correctly**
- [ ] ⏳ Correct colors matching MVS output

---

*Last updated: 2026-01-01 18:20*
*Next: Verify HDMI display during MVS capture, dump raw GPIO data*
